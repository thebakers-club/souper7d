/* ============================================================ frames ====
 * One scan of the shooting folder (plus ML/SOUP) gives every frame number
 * with its CR2/JPG and whether Soup has already developed it. ML's reported
 * prefix is unusable on this body (it stays "IMG_"), so files are matched by
 * number with the prefix ignored. More than one CR2 (or JPG) with one number
 * is ambiguous and that frame is refused rather than guessed.
 *
 * Outputs: SOUP####.JPG with the NEXT FREE number, because Canon playback
 * hides a JPG that shares its number with another image (DCF: one number,
 * one image). EXIF Software records the source folder and frame. Older outputs
 * with a bare "#nnnn" or no source tag count only in their own DCIM folder.
 * Canon only learns about our numbers
 * at its next boot, so it may reuse one for a shot meanwhile; the scan then
 * renames OUR file to a free number. Camera files are never renamed.
 *
 * In place (ONE JPEG / IN PLACE): the develop is written to SOUI####.TMP,
 * then Canon's JPG is removed and the TMP renamed to the JPG's name. A
 * developed JPG is known by "SOUP ENGINE" in its EXIF and is never used as
 * a source again. If the camera stopped between the remove and the rename,
 * the next scan finishes the rename.
 */
#define MAX_FRAMES 4096
#define F_CR2 1
#define F_JPG 2
#define F_DUP 4
#define F_TMP 8            /* SOUI####.TMP: an in-place develop not yet renamed */
typedef struct {uint16_t num;uint8_t flags,done;char cr2p[4],jpgp[4];} frame_ent;
static frame_ent *frames;
static int nframes, frames_overflow;
#define MAX_OUTS 1024
typedef struct {uint16_t num;uint8_t in_ml,ver;} out_ent;
static out_ent *outs;               /* lives in the same allocation as frames */
static int nouts, max_cam, max_ml;  /* highest numbers in use, camera folder / ML/SOUP */
static volatile int gui_task_alive;
/* The index is rebuilt on every use; holding 48 KB of Canon's heap between
 * uses is what Err 70 punishes. Freed by the GUI task on exit if no job runs,
 * else by the job task once the GUI task has fully exited. */
static void frames_release(void)
{
    frame_ent *f=frames;
    if(!f)return;
    frames=0;nframes=0;outs=0;nouts=0;
    free(f);
}
static char frames_dir[64];

static char up_ascii(char c){return (c>='a'&&c<='z')?(char)(c-32):c;}

static int dcim_folder_number(const char *folder)
{
    int i, number = 0;
    if (strlen(folder) != 8) return -1;
    for (i = 0; i < 3; i++) {
        if (folder[i] < '0' || folder[i] > '9') return -1;
        number = number * 10 + folder[i] - '0';
    }
    return number >= 100 ? number : -1;
}

/* Six hex digits hold all DCF folder/frame pairs. The folder name and
 * four-character file prefix distinguish renamed folders and custom prefixes. */
static void format_source_tag(char *tag, int cap, const char *stock, const char prefix[4], int num)
{
    const char *slash = strrchr(frames_dir, '/');
    const char *folder = slash ? slash + 1 : frames_dir;
    int number = dcim_folder_number(folder);
    static const char hex[] = "0123456789ABCDEF";
    char id[7], name[5];
    int i, code;

    if (number < 0) {
        snprintf(tag, cap, "SOUP ENGINE - %s #%04d", stock, num);
        return;
    }
    code = number * 10000 + num;
    for (i = 0; i < 6; i++) id[i] = hex[(code >> (20 - 4 * i)) & 15];
    id[6] = 0;
    memcpy(name, prefix, 4);
    name[4] = 0;
    snprintf(tag, cap, "SOUP ENGINE @%s/%s/%s - %s #%04d", id, folder, name, stock, num);
}

static int parse_dcim_name(const char *n,int *num,char ext[4])
{
    int i,v=0;
    if(strlen(n)!=12||n[8]!='.')return 0;
    for(i=0;i<4;i++){char c=up_ascii(n[i]);if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return 0;}
    for(i=4;i<8;i++){if(n[i]<'0'||n[i]>'9')return 0;v=v*10+(n[i]-'0');}
    for(i=0;i<3;i++)ext[i]=up_ascii(n[9+i]);
    ext[3]=0;*num=v;return v>=1;
}
/* 1 for SOUP, 2..9 for SOU2..SOU9, 0 for anything else */
static int soup_version(const char *n)
{
    if(up_ascii(n[0])!='S'||up_ascii(n[1])!='O'||up_ascii(n[2])!='U')return 0;
    if(up_ascii(n[3])=='P')return 1;
    if(n[3]>='2'&&n[3]<='9')return n[3]-'0';
    return 0;
}
static int frame_lookup(int num)
{
    int lo=0,hi=nframes-1;
    while(lo<=hi){int m=(lo+hi)/2;if(frames[m].num==num)return m;if(frames[m].num<num)lo=m+1;else hi=m-1;}
    return -1-lo;   /* insertion point, encoded */
}
static frame_ent *frame_get(int num)
{
    int at=frame_lookup(num);
    if(at>=0)return &frames[at];
    if(nframes>=MAX_FRAMES){frames_overflow=1;return 0;}
    at=-1-at;
    if(at<nframes)memmove(&frames[at+1],&frames[at],(size_t)(nframes-at)*sizeof(frame_ent));
    memset(&frames[at],0,sizeof(frame_ent));frames[at].num=(uint16_t)num;nframes++;
    return &frames[at];
}
static int scan_dir(const char *dir,int outputs_only)
{
    struct fio_file file;struct fio_dirent *d;int n,v;char ext[4];frame_ent *e;
    d=FIO_FindFirstEx(dir,&file);
    if(IS_ERROR(d))return -1;          /* directory handle: IS_ERROR is right here */
    do{
        if(stop_worker||ml_shutdown_requested)break;
        if(!file.name[0]||file.name[0]=='.'||(file.mode&ATTR_DIRECTORY))continue;
        if(!parse_dcim_name(file.name,&n,ext))continue;
        if(outputs_only){if(n>max_ml)max_ml=n;}else if(n>max_cam)max_cam=n;
        if(!outputs_only&&!strcmp(ext,"TMP")&&up_ascii(file.name[3])=='I'&&soup_version(file.name)==0&&
           up_ascii(file.name[0])=='S'&&up_ascii(file.name[1])=='O'&&up_ascii(file.name[2])=='U'){
            if((e=frame_get(n)))e->flags|=F_TMP;
            continue;
        }
        if(strcmp(ext,"CR2")&&strcmp(ext,"JPG"))continue;
        v=soup_version(file.name);
        if(v&&!strcmp(ext,"JPG")){
            if(nouts<MAX_OUTS){outs[nouts].num=(uint16_t)n;outs[nouts].in_ml=(uint8_t)outputs_only;outs[nouts].ver=(uint8_t)v;nouts++;}
            continue;
        }
        if(outputs_only)continue;
        if(!(e=frame_get(n)))continue;
        if(!strcmp(ext,"CR2")){if(e->flags&F_CR2)e->flags|=F_DUP;else{e->flags|=F_CR2;memcpy(e->cr2p,file.name,4);}}
        else{if(e->flags&F_JPG)e->flags|=F_DUP;else{e->flags|=F_JPG;memcpy(e->jpgp,file.name,4);}}
    }while(FIO_FindNextEx(d,&file)==0);
    FIO_FindClose(d);
    return 0;
}
static void out_path(const out_ent *o,int num,char *out,int cap,const char *ext)
{
    if(o->ver<=1)snprintf(out,cap,"%s/SOUP%04d.%s",o->in_ml?"ML/SOUP":frames_dir,num,ext);
    else snprintf(out,cap,"%s/SOU%d%04d.%s",o->in_ml?"ML/SOUP":frames_dir,o->ver,num,ext);
}
enum { SRC_INVALID = -1, SRC_UNTAGGED, SRC_LEGACY, SRC_QUALIFIED };
typedef struct { int num; char folder[9]; char prefix[4]; } source_ref;

static int parse_source_id(const uint8_t *s, int len, source_ref *ref)
{
    int code = 0, i, folder;
    if (len < 22 || s[7] != '/' || s[16] != '/' || s[21] != ' ') return SRC_INVALID;
    for (i = 1; i <= 6; i++) {
        char c = up_ascii(s[i]);
        if (c >= '0' && c <= '9') code = code * 16 + c - '0';
        else if (c >= 'A' && c <= 'F') code = code * 16 + c - 'A' + 10;
        else return SRC_INVALID;
    }
    memcpy(ref->folder, s + 8, 8);
    ref->folder[8] = 0;
    folder = dcim_folder_number(ref->folder);
    ref->num = code % 10000;
    if (folder < 0 || folder != code / 10000 || ref->num == 0) return SRC_INVALID;
    memcpy(ref->prefix, s + 17, 4);
    return SRC_QUALIFIED;
}

/* EXIF Software is close to the start of files written by our encoder. */
static int read_src(const char *path,uint8_t *buf,int cap, source_ref *ref)
{
    FILE *f;uint32_t size=0,n;int i;
    if(FIO_GetFileSize(path,&size)!=0||size<64)return SRC_INVALID;
    n=size<(uint32_t)cap?size:(uint32_t)cap;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))return SRC_INVALID;
    if(read_exact(f,0,buf,n)!=0){FIO_CloseFile(f);return SRC_INVALID;}
    FIO_CloseFile(f);
    for(i=0;i+11<(int)n;i++){
        int end;
        if(memcmp(buf+i,"SOUP ENGINE",11))continue;
        end=i+11;
        while(end<(int)n&&buf[end])end++;
        if(end>i+12&&buf[i+11]==' '&&buf[i+12]=='@')
            return parse_source_id(buf+i+12,end-i-12,ref);
        for(i+=11;i+4<end;i++){
            if(buf[i]=='#'&&buf[i+1]>='0'&&buf[i+1]<='9'&&buf[i+2]>='0'&&buf[i+2]<='9'&&
               buf[i+3]>='0'&&buf[i+3]<='9'&&buf[i+4]>='0'&&buf[i+4]<='9'){
                ref->num=(buf[i+1]-'0')*1000+(buf[i+2]-'0')*100+(buf[i+3]-'0')*10+(buf[i+4]-'0');
                return ref->num>=1?SRC_LEGACY:SRC_INVALID;
            }
        }
        break;
    }
    return SRC_UNTAGGED;
}

static int same_name(const char *a, const char *b, int len)
{
    int i;
    for (i = 0; i < len; i++) if (up_ascii(a[i]) != up_ascii(b[i])) return 0;
    return 1;
}
/* 1 if the file's first `cap` bytes carry our EXIF Software tag. */
static int soup_marked(const char *path,uint8_t *buf,int cap)
{
    FILE *f;uint32_t size=0,n;int i;
    if(FIO_GetFileSize(path,&size)!=0||size<64)return 0;
    n=size<(uint32_t)cap?size:(uint32_t)cap;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))return 0;
    i=read_exact(f,0,buf,n);FIO_CloseFile(f);
    if(i!=0)return 0;
    for(i=0;i+11<=(int)n;i++)if(!memcmp(buf+i,"SOUP ENGINE",11))return 1;
    return 0;
}
/* A whole JPEG: SOI first, EOI last. */
static int jpeg_complete(const char *path)
{
    FILE *f;uint32_t size=0;uint8_t b[4];int ok=0;
    if(FIO_GetFileSize(path,&size)!=0||size<4)return 0;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f))return 0;
    if(read_exact(f,0,b,2)==0&&read_exact(f,size-2,b+2,2)==0)
        ok=b[0]==0xFF&&b[1]==0xD8&&b[2]==0xFF&&b[3]==0xD9;
    FIO_CloseFile(f);return ok;
}
static void frame_path(const frame_ent *e,int cr2,char *out,int cap);
static void inplace_tmp(int num,char *out,int cap){snprintf(out,cap,"%s/SOUI%04d.TMP",frames_dir,num);}

/* Next unused output number. Camera folder: above everything in the folder
 * AND above Canon's counter (the last shot it took). -1 when past 9999. */
static int next_output(int in_ml,char *out,char *tmp,int cap)
{
    struct card_info *card=get_shooting_card();
    int n=in_ml?max_ml:max_cam;
    const char *dir=in_ml?"ML/SOUP":frames_dir;
    if(!in_ml&&card&&card->file_number>n)n=card->file_number;
    for(n++;n<=9999;n++){
        snprintf(out,cap,"%s/SOUP%04d.JPG",dir,n);snprintf(tmp,cap,"%s/SOUP%04d.TMP",dir,n);
        if(!is_file(out)&&!is_file(tmp)){if(in_ml)max_ml=n;else max_cam=n;return n;}
    }
    return -1;
}
/* Rebuild the index for the current shooting folder. Never while a job runs. */
static int index_build(void)
{
    int i,j,at;uint8_t *buf;char p[96],q[96],tq[96];
    const char *folder;
    if(!frames){
        frames=malloc(MAX_FRAMES*sizeof(frame_ent)+MAX_OUTS*sizeof(out_ent));if(!frames)return -1;
        outs=(out_ent*)(frames+MAX_FRAMES);
    }
    nframes=0;frames_overflow=0;nouts=0;max_cam=0;max_ml=0;
    snprintf(frames_dir,sizeof(frames_dir),"%s",get_dcim_dir());
    folder=strrchr(frames_dir,'/');
    folder=folder?folder+1:frames_dir;
    if(scan_dir(frames_dir,0)!=0)return -1;
    scan_dir("ML/SOUP",1);             /* outputs kept there count as developed */
    buf=fio_malloc(1024);
    for(i=0;i<nouts;i++){
        out_ent *o=&outs[i];
        source_ref ref;
        int kind, source_at=-1;
        if(!buf)break;
        out_path(o,o->num,p,sizeof(p),"JPG");
        kind=read_src(p,buf,1024,&ref);
        if(kind==SRC_QUALIFIED&&strlen(folder)==8&&same_name(folder,ref.folder,8)){
            at=frame_lookup(ref.num);
            if(at>=0&&(((frames[at].flags&F_CR2)&&same_name(frames[at].cr2p,ref.prefix,4))||
                       ((frames[at].flags&F_JPG)&&same_name(frames[at].jpgp,ref.prefix,4))))source_at=at;
        }else if(!o->in_ml&&(kind==SRC_LEGACY||kind==SRC_UNTAGGED)){
            source_at=frame_lookup(kind==SRC_LEGACY?ref.num:o->num);
        }
        /* Canon reused our number for a real shot: move OUR file out of the way */
        if(!o->in_ml&&(kind==SRC_QUALIFIED||(kind==SRC_LEGACY&&ref.num!=o->num))&&
           (at=frame_lookup(o->num))>=0&&at!=source_at&&(frames[at].flags&(F_CR2|F_JPG))){
            int nn=next_output(0,q,tq,sizeof(q));
            if(nn>0){
                out_ent t=*o;t.ver=1;out_path(&t,nn,q,sizeof(q),"JPG");
                if(FIO_RenameFile(p,q)==0){char d_[224];snprintf(d_,sizeof(d_),"renamed %s -> %s (camera reused the number)",p,q);diag(d_);diag_flush();o->num=(uint16_t)nn;o->ver=1;}
            }
        }
        if(source_at>=0)frames[source_at].done=1;
    }
    /* Interrupted in-place swaps: the TMP is the develop, the JPG is gone.
     * Finish the rename -- only for a complete JPEG; a cut-off one is a
     * develop that died writing (a RAW-only frame still has its CR2). A
     * TMP beside a JPG never got as far as the swap: the JPG is the
     * original. */
    for(i=0;i<nframes;i++){
        frame_ent *e=&frames[i];
        if(!(e->flags&F_TMP))continue;
        inplace_tmp(e->num,tq,sizeof(tq));
        if(!(e->flags&F_JPG)&&jpeg_complete(tq)){
            if(!(e->flags&F_CR2))memcpy(e->jpgp,"IMG_",4);else memcpy(e->jpgp,e->cr2p,4);
            frame_path(e,0,q,sizeof(q));
            if(FIO_RenameFile(tq,q)==0){e->flags|=F_JPG;e->done=1;
                {char d_[224];snprintf(d_,sizeof(d_),"recovered %s -> %s",tq,q);diag(d_);diag_flush();}}
        }else FIO_RemoveFile(tq);
    }
    for(i=j=0;i<nframes;i++)if(frames[i].flags&(F_CR2|F_JPG))frames[j++]=frames[i];
    nframes=j;
    /* Developed in place: the frame's own JPG carries our tag. Only the
     * newest such frame matters (ALL NEW starts after it), so read back
     * from the newest frame until one is found or a frame is already known
     * done -- at most 200 headers of 1 KB (a job takes 200 frames). */
    if(buf){
        int k=0;
        for(i=nframes-1;i>=0&&k<200;i--){
            frame_ent *e=&frames[i];
            if(e->done)break;
            if(!(e->flags&F_JPG)||(e->flags&F_DUP))continue;
            frame_path(e,0,p,sizeof(p));k++;
            if(soup_marked(p,buf,1024)){e->done=1;break;}
        }
        fio_free(buf);
    }
    return 0;
}
static void frame_path(const frame_ent *e,int cr2,char *out,int cap)
{
    /* No %c: Canon's firmware snprintf mangles it (repeats the first char
     * to the end of the buffer). Build the prefix as a string instead. */
    char p[5];
    memcpy(p,cr2?e->cr2p:e->jpgp,4);p[4]=0;
    snprintf(out,cap,"%s/%s%04d.%s",frames_dir,p,e->num,cr2?"CR2":"JPG");
}
/* The camera's last shot if it is in this folder, else the highest frame. */
static int latest_index(void)
{
    struct card_info *card=get_shooting_card();
    int at=card?frame_lookup(card->file_number):-1;
    if(at>=0)return at;
    return nframes-1;
}
/* Frames shot after the newest one Soup has developed in this folder. If
 * nothing here has been developed yet, "new" is just the latest frame:
 * a first use must never queue the whole card. */
static int select_new(uint16_t *out,int cap)
{
    int i,last=-1,n=0;
    for(i=0;i<nframes;i++)if(frames[i].done)last=i;
    if(last<0){i=latest_index();if(i>=0&&cap>0){out[0]=frames[i].num;return 1;}return 0;}
    for(i=last+1;i<nframes&&n<cap;i++)if(!frames[i].done)out[n++]=frames[i].num;
    return n;
}
static int select_last(uint16_t *out,int cap,int count)
{
    int i,n=0,start;
    if(count>cap)count=cap;
    start=nframes-count;if(start<0)start=0;
    for(i=start;i<nframes;i++)out[n++]=frames[i].num;
    return n;
}
