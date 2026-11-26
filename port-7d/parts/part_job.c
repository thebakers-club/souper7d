/* ================================================================ jobs ===
 * A job is a list of frame numbers developed one after another with one
 * stock and the settings as they were when it started. Per frame:
 *   source  Canon's JPG when it is exactly the output size (decoded 1:1 --
 *           RAW+S at HALF: a quarter of the decode work) or twice it
 *           (decoded at half), otherwise the CR2's embedded preview. The
 *           output size is the frame's largest picture, halved for HALF.
 *           Both are read-only.
 *   output  ONE JPEG (default) / IN PLACE: the frame's own JPG name, which
 *           Canon playback already lists. Written as SOUI####.TMP, verified,
 *           then Canon's JPG removed and the TMP renamed over it; ONE JPEG
 *           then removes the CR2 too. A JPG we developed is never a source:
 *           only the CR2 can be developed again.
 *           NEW FILE: SOUP#### with the next free number, in the shooting
 *           folder or ML/SOUP; never an overwrite.
 *   delete  NEW FILE + DELETE RAW: the CR2 only, only when developed from a
 *           JPG of the same frame at least as large as the CR2 preview, only after the output re-reads complete.
 */
#define MAX_JOB 200
static uint16_t job_nums[MAX_JOB];
static int job_total;
static volatile int job_pos;
static int job_ok, job_failed, job_skipped, job_deleted, job_kept;
static int job_out;                    /* NEW FILE modes: 0 camera folder, 1 ML/SOUP */
static int auto_base=-1, auto_upto, auto_upto_folder;
static int lv_ago=99;                  /* seconds since Live View was last on */
static const char *const mode_names[M_COUNT]={"JPEG ONLY","REPLACE JPEG","NEW JPEG","NEW JPEG + DELETE RAW","ML/SOUP FOLDER"};
static const char *const halation_names[HALATION_COUNT]={"STOCK","SKIP EMPTY","OFF"};
static const char *const cache_names[SE_LOOK_CACHE_COUNT]={"4K","8K","16K"};
static char last_out[96], last_from[4];
static char job_log[2048]; static int job_log_len;

static void log_line(const char *s)
{
    int n=strlen(s);
    if(job_log_len+n+2>=(int)sizeof(job_log))return;
    memcpy(job_log+job_log_len,s,n);job_log_len+=n;
    job_log[job_log_len++]='\r';job_log[job_log_len++]='\n';job_log[job_log_len]=0;
}

typedef struct {int is_cr2;uint32_t size,offset,length;size_t hdr_len;se_jdec_info ji;} source_info;
/* Read the start of a CR2 or JPG into `header` and locate the JPEG stream:
 * the embedded full-size preview for a CR2, the whole file for a JPG. */
static void probe_log(const char *path,const char *step,int v)
{
    char t[128];snprintf(t,sizeof(t),"probe %s: %s (%d)",path,step,v);diag(t);log_line(t);
}
static int probe_source(const char *path,int is_cr2,void *header,source_info *s,uint32_t *got)
{
    FILE *f;uint32_t n;int pw,ph,rc;
    memset(s,0,sizeof(*s));s->is_cr2=is_cr2;*got=0;
    if((rc=FIO_GetFileSize(path,&s->size))!=0){probe_log(path,"getsize rc",rc);return -1;}
    if(s->size<64){probe_log(path,"too small",(int)s->size);return -1;}
    if(is_cr2&&s->size<HDR_CAP){probe_log(path,"cr2 too small",(int)s->size);return -1;}
    n=s->size<HDR_CAP?s->size:HDR_CAP;
    f=FIO_OpenFile(path,O_RDONLY);if(!good_file(f)){probe_log(path,"open failed",(int)f);return -1;}
    if(read_exact(f,0,header,n)!=0){FIO_CloseFile(f);probe_log(path,"read failed n",(int)n);return -1;}
    FIO_CloseFile(f);*got=n;
    if(is_cr2){if((rc=se_cr2_find_preview(header,n,&s->offset,&s->length,&pw,&ph))!=SE_OK){probe_log(path,"cr2 preview rc",rc);return -1;}}
    else{s->offset=0;s->length=s->size;}
    if(!s->length||s->length>MAX_PREVIEW||s->offset>=n||s->offset>s->size||s->length>s->size-s->offset){probe_log(path,"bad bounds len",(int)s->length);return -1;}
    s->hdr_len=n-s->offset;if(s->hdr_len>s->length)s->hdr_len=s->length;
    if((rc=se_jpeg_dec_info((uint8_t*)header+s->offset,s->hdr_len,&s->ji))!=SE_OK){probe_log(path,"dec_info rc",rc);return -1;}
    if(s->ji.width<32||s->ji.width>6000||s->ji.height<16||s->ji.height>4500){probe_log(path,"bad dims w",s->ji.width);return -1;}
    return 0;
}
/* Re-read a finished output: exact size, SOI first, EOI last. */
static int verify_jpeg(const char *path,uint32_t size)
{
    FILE *f;uint8_t *b;uint32_t got=0;int ok=-1;
    if(size<4||FIO_GetFileSize(path,&got)!=0||got!=size)return -1;
    b=fio_malloc(64);if(!b)return -1;
    f=FIO_OpenFile(path,O_RDONLY);
    if(good_file(f)){
        if(read_exact(f,0,b,2)==0&&b[0]==0xFF&&b[1]==0xD8&&
           read_exact(f,size-2,b+2,2)==0&&b[2]==0xFF&&b[3]==0xD9)ok=0;
        FIO_CloseFile(f);
    }
    fio_free(b);return ok;
}

/* The output's EXIF thumbnail: the source's own 160x120 thumbnail run
 * through the same look and re-encoded small, so Canon's index view shows
 * the graded frame rather than the original. Optional: any failure just
 * means no thumbnail. Uses its own short-lived allocation, freed before
 * the main develop allocates. */
#define TH_CAP 7000                    /* must fit the encoder's 8 KB EXIF block */
typedef struct {uint8_t *buf;size_t len,cap;} membuf;
static int mem_sink(void *ctx,const uint8_t *d,size_t n)
{
    membuf *m=ctx;
    if(n>m->cap-m->len)return -1;
    memcpy(m->buf+m->len,d,n);m->len+=n;return 0;
}
/* Set to 1 for per-batch JPEG timing; the outer encode timer runs in both builds. */
enum { JPEG_DETAIL_TIMING = 0 };
enum {
    STREAM_DECODE, STREAM_LOOK, STREAM_ENCODE, STREAM_READ, STREAM_WRITE,
    STREAM_YIELD, STREAM_JPEG_COLOR, STREAM_JPEG_TRANSFORM, STREAM_JPEG_ENTROPY,
    STREAM_PHASE_COUNT
};

typedef struct {
    unsigned ms[STREAM_PHASE_COUNT];
    unsigned since;
    int phase;
} stream_timing;

/* One active phase at a time, even during nested encoder and card calls. */
static void stream_mark_at(stream_timing *timing, int phase, unsigned now)
{
    timing->ms[timing->phase] += now - timing->since;
    timing->since = now;
    timing->phase = phase;
}

static void stream_mark(stream_timing *timing, int phase)
{
    stream_mark_at(timing, phase, (unsigned)get_ms_clock());
}

typedef struct {
    se_jpeg *enc;
    se_look_run *look;
    int width;
    stream_timing *timing;
} pipeline;
static int th_emit(void *ctx,const uint8_t *row,int width)
{
    pipeline *p=ctx;
    if(width!=p->width)return -1;
    return se_jpeg_write_rows(p->enc,row,1)==SE_OK?0:-1;
}
static int th_rows(void *ctx,const uint8_t *rgb,int rows,int width)
{
    pipeline *p=ctx;
    if(JOB_STOP()||width!=p->width)return -1;
    return se_look_rows(p->look,rgb,rows,th_emit,p)==SE_OK?0:-1;
}
static void job_look_from_stock(const se_stock *stock, int scale, se_look *look)
{
    se_look_from_stock(stock,scale,look);
    if(job_render.halation==HALATION_OFF)look->hal_amount=0;
    if(!job_render.grain)look->grain=0;
}

static int graded_thumb(const se_stock *stock,const uint8_t *jpg,uint32_t len,uint8_t *out,int src_width)
{
    se_arena a,la;se_jdec_info ji;se_look lk;pipeline p;membuf mb;uint8_t *mem,*strip;
    size_t ls;int result=0;
    if(!jpg||len<64)return 0;
    memset(&p,0,sizeof(p));
    if(se_jpeg_dec_info(jpg,len,&ji)!=SE_OK||ji.width>THUMB_W||ji.width<16)return 0;
    /* grain and halation are scaled to the thumbnail's size, not the frame's */
    (void)src_width;
    job_look_from_stock(stock,4,&lk);   /* coarsest supported scale: grain and glow stay small */
    ls=se_look_fit(&lk,ji.width,60*1024);
    mem=tmp_malloc(96*1024+ls);if(!mem)return 0;
    se_arena_init(&a,mem,96*1024);
    se_arena_init(&la,mem+96*1024,ls);
    p.enc=SE_NEW(&a,se_jpeg,1);strip=SE_NEW(&a,uint8_t,(size_t)8*ji.width*3);p.width=ji.width;
    if(!p.enc||!strip||se_look_begin_with_options(&p.look,&lk,ji.width,&la,SE_LOOK_CACHE_4K,
                                               job_render.halation==HALATION_SKIP_EMPTY)!=SE_OK)goto out;
    mb.buf=out;mb.len=0;mb.cap=TH_CAP;
    if(se_jpeg_begin(p.enc,ji.width,ji.height,75,strip,0,mem_sink,&mb)!=SE_OK)goto out;
    if(se_jpeg_decode_scaled(jpg,len,1,&a,th_rows,&p,&ji)!=SE_OK)goto out;
    if(se_look_end(p.look,th_emit,&p)!=SE_OK||se_jpeg_end(p.enc)!=SE_OK)goto out;
    result=(int)mb.len;
out:
    free(mem);return result;
}

typedef struct {
    FILE *f;
    uint32_t base,total,next,header_len,card_bytes,seeks;
    const uint8_t *header;
    void *dma;
    stream_timing *timing;
} reader;
static int preview_read(void *ctx,uint32_t off,void *dst,uint32_t n)
{
    reader *r=ctx;
    uint32_t end=off+n, cached=0, done=0;
    if(JOB_STOP()||!r->total||n>IO_CAP||off>r->total||n>r->total-off)return -1;
    if(off<r->header_len){
        cached=SE_MIN(n,r->header_len-off);
        memcpy(dst,r->header+off,cached);
    }
    if(cached<n){
        uint32_t pos=r->base+off+cached, rest=n-cached;
        /* JPEG reads forward. Keep the file cursor between decoder refills. */
        stream_mark(r->timing,STREAM_READ);
        if(pos!=r->next){
            if(FIO_SeekSkipFile(r->f,pos,SEEK_SET)!=pos)return -1;
            r->next=pos;r->seeks++;
        }
        while(done<rest){
            int got=FIO_ReadFile(r->f,(uint8_t*)r->dma+done,rest-done);
            if(got<=0||(uint32_t)got>rest-done)return -1;
            done+=(uint32_t)got;
            r->next+=(uint32_t)got;
        }
        stream_mark(r->timing,STREAM_DECODE);
        r->card_bytes+=rest;
        memcpy((uint8_t*)dst+cached,r->dma,rest);
    }
    /* MAX_PREVIEW is 32 MiB, so the product fits in 32 bits. */
    progress_pct=(int)(end*100u/r->total);
    return 0;
}
typedef struct {
    FILE *f;
    void *dma;
    uint32_t used,total;
    int error;
    stream_timing *timing;
} writer;

static void jpeg_mark_phase(void *ctx,se_jpeg_phase phase)
{
    static const int phases[]={
        [SE_JPEG_COLOR]=STREAM_JPEG_COLOR, [SE_JPEG_TRANSFORM]=STREAM_JPEG_TRANSFORM,
        [SE_JPEG_ENTROPY]=STREAM_JPEG_ENTROPY, [SE_JPEG_OTHER]=STREAM_ENCODE
    };
    writer *w=ctx;
    stream_mark(w->timing,phases[phase]);
}

static int flush_output(writer *w)
{
    uint32_t done=0;
    if(!w->used)return 0;
    int parent=w->timing->phase;
    stream_mark(w->timing,STREAM_WRITE);
    while(done<w->used){
        int n=FIO_WriteFile(w->f,(uint8_t*)w->dma+done,w->used-done);
        if(n<=0||(uint32_t)n>w->used-done){w->error=1;return -1;}
        done+=(uint32_t)n;
    }
    stream_mark(w->timing,parent);
    w->total+=w->used;w->used=0;return 0;
}
static int jpeg_sink(void *ctx,const uint8_t *data,size_t len)
{
    writer *w=ctx;
    if(JOB_STOP()||w->error)return -1;
    while(len){
        size_t n=IO_CAP-w->used;if(n>len)n=len;
        memcpy((uint8_t*)w->dma+w->used,data,n);
        w->used+=n;data+=n;len-=n;
        if(w->used==IO_CAP&&flush_output(w)!=0)return -1;
    }
    return 0;
}
static int emit_row(void *ctx,const uint8_t *row,int width)
{
    pipeline *p=ctx;
    int rc;
    if(JOB_STOP()||width!=p->width)return -1;
    /* Only the eighth row runs the encoder. The other calls just queue RGB. */
    if(p->enc->strip_rows!=7)return se_jpeg_write_rows(p->enc,row,1)==SE_OK?0:-1;
    stream_mark(p->timing,STREAM_ENCODE);
    rc=se_jpeg_write_rows(p->enc,row,1);
    stream_mark(p->timing,STREAM_LOOK);
    return rc==SE_OK?0:-1;
}
static int grade_rows(void *ctx,const uint8_t *rgb,int rows,int width)
{
    pipeline *p=ctx;
    static unsigned last_yield;
    unsigned now;
    int rc;
    if(JOB_STOP()||width!=p->width)return -1;
    /* Yield so Canon's lower-priority tasks keep running (the GUI task
     * outranks this one and preempts it anyway). msleep(1) costs a whole
     * 10 ms DryOS tick: once per MCU row was 2.3 s a frame, so only after
     * ~500 ms of work. */
    now=(unsigned)get_ms_clock();
    if(now-last_yield>=500){
        stream_mark_at(p->timing,STREAM_YIELD,now);
        msleep(1);
        now=(unsigned)get_ms_clock();
        last_yield=now;
    }
    stream_mark_at(p->timing,STREAM_LOOK,now);
    rc=se_look_rows(p->look,rgb,rows,emit_row,p);
    stream_mark(p->timing,STREAM_DECODE);
    return rc==SE_OK?0:-1;
}

/* Our EXIF Software tag sits in the first few hundred bytes of a JPG. */
static int has_tag(const void *h,uint32_t n)
{
    const uint8_t *b=h;uint32_t i;
    if(n>4096)n=4096;
    for(i=0;i+11<=n;i++)if(b[i]=='S'&&!memcmp(b+i,"SOUP ENGINE",11))return 1;
    return 0;
}

/* Develop one frame. 1 ok, 0 skipped, -1 failed. */
static int develop_frame(int num,const se_stock *stock,char *text_scratch)
{
    FILE *in=0,*out=0;void *header=0,*memory=0,*read_dma=0,*write_dma=0;
    uint32_t check_size=0,got=0;
    int at,ow=0,oh=0,success=0,created=0,have_cr2,scale=job_scale,raw_deleted=0,thumb_len=0,version=0;
    int inplace=job_mode<=M_INPLACE,jpg_soup=0,skipped=0,keep_temp=0;
    se_err rc=SE_E_IO;se_look look;se_jpeg_exif ex;meta_info meta;
    se_arena arena,look_arena;pipeline pipe;source_info src,alt;
    reader rd={0};writer wr;size_t look_size;uint8_t *strip,*thumb=0;
    frame_ent *e;char cr2_path[96],jpg_path[96],input_path[96],output_path[96],temp_path[96],sw[96];
    const char *why="failed";
    int t_start=get_ms_clock();
    unsigned phase_start,probe_ms,thumb_ms,stream_ms,commit_ms;
    unsigned stream_start;
    stream_timing timing={0};
    se_look_timing look_timing={.clock_ms=get_ms_clock};
    int cache=job_render.cache;
    size_t look_extra;
    memset(&pipe,0,sizeof(pipe));memset(&wr,0,sizeof(wr));memset(&src,0,sizeof(src));memset(&ex,0,sizeof(ex));
    memset(&meta,0,sizeof(meta));input_path[0]=0;
    at=frame_lookup(num);
    if(at<0){snprintf(text_scratch,128,"%04d not found",num);log_line(text_scratch);return 0;}
    e=&frames[at];
    if(e->flags&F_DUP){snprintf(text_scratch,128,"%04d skipped: several files with this number",num);log_line(text_scratch);return 0;}
    /* a RAW-only frame gets its JPG under the CR2's prefix */
    if(!(e->flags&F_JPG))memcpy(e->jpgp,e->cr2p,4);
    frame_path(e,1,cr2_path,sizeof(cr2_path));frame_path(e,0,jpg_path,sizeof(jpg_path));
    if(inplace){
        snprintf(output_path,sizeof(output_path),"%s",jpg_path);
        inplace_tmp(num,temp_path,sizeof(temp_path));
        if(is_file(temp_path))FIO_RemoveFile(temp_path);   /* ours, from a failed try */
    }else if((version=next_output(job_out,output_path,temp_path,sizeof(output_path)))<0){
        snprintf(text_scratch,128,"%04d skipped: no free file number (9999 reached)",num);log_line(text_scratch);return 0;}
    header=fio_malloc(HDR_CAP);if(!header){why="no mem header";goto done;}
    have_cr2=(e->flags&F_CR2)&&probe_source(cr2_path,1,header,&alt,&got)==0;
    if((e->flags&F_JPG)&&probe_source(jpg_path,0,header,&src,&got)==0&&!(jpg_soup=has_tag(header,got))){
        int fw=src.ji.width,fh=src.ji.height,tw,th,w2,h2;
        if(have_cr2&&alt.ji.width>fw){fw=alt.ji.width;fh=alt.ji.height;}
        se_jpeg_scaled_size(fw,fh,job_scale,&tw,&th);
        se_jpeg_scaled_size(src.ji.width,src.ji.height,2,&w2,&h2);
        scale=(src.ji.width==tw&&src.ji.height==th)?1:(w2==tw&&h2==th)?2:0;
        if(scale)snprintf(input_path,sizeof(input_path),"%s",jpg_path);
    }
    if(!input_path[0]){
        scale=job_scale;
        /* never grade our own output a second time */
        if(jpg_soup&&!have_cr2){why="already developed";skipped=1;goto done;}
        if(!have_cr2){why="unreadable";goto done;}
        if(e->flags&F_JPG){
            if(probe_source(cr2_path,1,header,&src,&got)!=0){why="unreadable";goto done;}
        }else src=alt;  /* RAW-only: our first probe is still in the header buffer */
        snprintf(input_path,sizeof(input_path),"%s",cr2_path);
    }
    read_meta(header,got,src.is_cr2,&meta);
    phase_start=(unsigned)get_ms_clock();
    probe_ms=phase_start-(unsigned)t_start;
    /* graded EXIF thumbnail, from the header already in memory */
    thumb=malloc(TH_CAP);
    if(thumb&&meta.thumb_len)thumb_len=graded_thumb(stock,(uint8_t*)header+meta.thumb_off,meta.thumb_len,thumb,src.ji.width);
    format_source_tag(sw,sizeof(sw),stock_names[job_stock],src.is_cr2?e->cr2p:e->jpgp,num);
    ex.make=meta.make[0]?meta.make:"Canon";ex.model=meta.model[0]?meta.model:"Canon EOS 7D";
    ex.software=sw;ex.datetime=meta.date[0]?meta.date:0;
    ex.exposure_time=meta.exposure;ex.aperture=meta.fnumber;ex.iso=meta.iso;ex.orientation=meta.orient;
    ex.thumb=thumb_len>0?thumb:0;ex.thumb_len=thumb_len;
    thumb_ms=(unsigned)get_ms_clock()-phase_start;

    in=FIO_OpenFile(input_path,O_RDONLY);if(!good_file(in)){in=0;goto done;}
    se_jpeg_scaled_size(src.ji.width,src.ji.height,scale,&ow,&oh);
    /* grain and glow are sized against the full frame, not the source */
    job_look_from_stock(stock,job_scale,&look);
    look_size=se_look_fit(&look,ow,SE_LOOK_BUDGET);
    if(!look.hue_any&&look.sat==0)cache=SE_LOOK_CACHE_4K;
    for(;;){
        look_extra=se_look_cache_extra(cache);
        memory=tmp_malloc(SCRATCH_CAP+look_size+look_extra+64);
        if(memory||cache==SE_LOOK_CACHE_4K)break;
        cache--;
    }
    read_dma=fio_malloc(IO_CAP);write_dma=fio_malloc(IO_CAP);
    if(!memory||!read_dma||!write_dma){why=!memory?"no mem scratch":!read_dma?"no mem read_dma":"no mem write_dma";goto done;}
    se_arena_init(&arena,memory,SCRATCH_CAP);
    se_arena_init(&look_arena,(uint8_t*)memory+SCRATCH_CAP,look_size+look_extra);
    pipe.enc=SE_NEW(&arena,se_jpeg,1);pipe.width=ow;
    strip=SE_NEW(&arena,uint8_t,(size_t)8*ow*3);
    if(!pipe.enc||!strip)goto done;
    if(se_look_begin_with_options(&pipe.look,&look,ow,&look_arena,cache,
                                 job_render.halation==HALATION_SKIP_EMPTY)!=SE_OK)goto done;
    se_look_set_timing(pipe.look,&look_timing);
    if((!inplace&&is_file(output_path))||is_file(temp_path))goto done;
    /* O_EXCL is not assumed supported by Canon's FIO; names are chosen
     * unused and rechecked immediately, and only this task writes them. */
    out=FIO_CreateFile(temp_path);if(!good_file(out)){out=0;why="cannot create output";goto done;}created=1;
    wr.f=out;wr.dma=write_dma;wr.timing=&timing;
    rd.f=in;rd.base=src.offset;rd.total=src.length;rd.dma=read_dma;
    rd.header=(uint8_t*)header+src.offset;rd.header_len=(uint32_t)src.hdr_len;
    rd.timing=&timing;pipe.timing=&timing;
    stream_start=(unsigned)get_ms_clock();
    timing.since=stream_start;
    timing.phase=STREAM_ENCODE;
    if(se_jpeg_begin(pipe.enc,ow,oh,88,strip,&ex,jpeg_sink,&wr)!=SE_OK)goto done;
    if(JPEG_DETAIL_TIMING)pipe.enc->mark_phase=jpeg_mark_phase;
    stream_mark(&timing,STREAM_DECODE);
    rc=se_jpeg_decode_stream_scaled((uint8_t*)header+src.offset,src.hdr_len,src.length,
        preview_read,&rd,scale,&arena,grade_rows,&pipe,&src.ji);
    if(rc!=SE_OK)goto done;
    stream_mark(&timing,STREAM_LOOK);
    if(se_look_end(pipe.look,emit_row,&pipe)!=SE_OK)goto done;
    stream_mark(&timing,STREAM_ENCODE);
    if(se_jpeg_end(pipe.enc)!=SE_OK||flush_output(&wr)!=0||wr.error)goto done;
    phase_start=(unsigned)get_ms_clock();
    stream_mark_at(&timing,STREAM_ENCODE,phase_start);
    stream_ms=phase_start-stream_start;
    FIO_CloseFile(out);out=0;
    FIO_CloseFile(in);in=0;            /* the source may be the file being replaced */
    if(FIO_GetFileSize(temp_path,&check_size)!=0||check_size!=wr.total||!check_size)goto done;
    if(inplace){
        if(verify_jpeg(temp_path,wr.total)!=0){why="verify failed";goto done;}
        if(JOB_STOP())goto done;
        if(is_file(output_path)&&(FIO_RemoveFile(output_path)!=0||is_file(output_path))){why="cannot remove camera jpg";goto done;}
        /* From here the TMP is the frame's only picture: it is never removed
         * on failure, and the next scan finishes the rename. */
        keep_temp=1;
        if(FIO_RenameFile(temp_path,output_path)!=0){why="rename failed, SOUI tmp kept";goto done;}
        keep_temp=0;
        if(FIO_GetFileSize(output_path,&check_size)!=0||check_size!=wr.total){why="size check after rename";goto done;}
        success=1;
        e->flags|=F_JPG;
        if(job_mode==M_ONE&&(e->flags&F_CR2)){
            if(FIO_RemoveFile(cr2_path)==0&&!is_file(cr2_path)){raw_deleted=1;job_deleted++;e->flags&=~F_CR2;}
            else job_kept++;
        }
        goto done;
    }
    if(is_file(output_path))goto done;
    if(FIO_RenameFile(temp_path,output_path)!=0){why="rename failed";goto done;}
    if(FIO_GetFileSize(output_path,&check_size)!=0||check_size!=wr.total)goto done;
    success=1;
    if(job_delete&&have_cr2&&!JOB_STOP()){
        /* no full-size JPG (none, or RAW+S): the CR2 is the original */
        if(src.is_cr2||!have_cr2||src.ji.width<alt.ji.width)job_kept++;
        else if(verify_jpeg(output_path,wr.total)==0&&is_file(jpg_path)&&
                FIO_RemoveFile(cr2_path)==0&&!is_file(cr2_path)){raw_deleted=1;job_deleted++;}
        else job_kept++;
    }
done:
    if(out)FIO_CloseFile(out);
    if(in)FIO_CloseFile(in);
    if(!success&&created&&!keep_temp&&is_file(temp_path))FIO_RemoveFile(temp_path);
    if(header)fio_free(header);
    if(read_dma)fio_free(read_dma);
    if(write_dma)fio_free(write_dma);
    if(memory)free(memory);
    if(thumb)free(thumb);
    if(success){
        commit_ms=(unsigned)get_ms_clock()-phase_start;
        snprintf(last_out,sizeof(last_out),"%s",output_path);
        snprintf(last_from,sizeof(last_from),"%s",src.is_cr2?"CR2":"JPG");
        e->done=1;(void)version;
    }
    snprintf(text_scratch,256,"%04d %s from=%s out=%s scale=%d thumb=%d orient=%d raw_deleted=%d ms=%d",
        num,success?"ok":skipped?"skipped: already developed (no CR2 left)":JOB_STOP()?"cancelled":why,input_path[0]?input_path:"-",
        success?output_path:"-",scale,thumb_len,meta.orient,raw_deleted,get_ms_clock()-t_start);
    log_line(text_scratch);
    if(success){
        unsigned look_accounted=look_timing.grade_ms+look_timing.mask_ms+
                                look_timing.glow_ms+look_timing.grain_ms;
        unsigned encode_ms=timing.ms[STREAM_ENCODE]+timing.ms[STREAM_JPEG_COLOR]+
                           timing.ms[STREAM_JPEG_TRANSFORM]+timing.ms[STREAM_JPEG_ENTROPY];
        /* Canon's snprintf does not handle %u. These counters fit in int. */
        snprintf(text_scratch,256,"perf %04d probe_ms=%d thumb_ms=%d stream_ms=%d commit_ms=%d seeks=%d stream_read_bytes=%d",
                 num,(int)probe_ms,(int)thumb_ms,(int)stream_ms,(int)commit_ms,(int)rd.seeks,(int)rd.card_bytes);
        log_line(text_scratch);
        snprintf(text_scratch,256,"pipe %04d decode_ms=%d look_ms=%d encode_ms=%d read_ms=%d write_ms=%d yield_ms=%d",
                 num,(int)timing.ms[STREAM_DECODE],(int)timing.ms[STREAM_LOOK],(int)encode_ms,
                 (int)timing.ms[STREAM_READ],(int)timing.ms[STREAM_WRITE],(int)timing.ms[STREAM_YIELD]);
        log_line(text_scratch);
        if(JPEG_DETAIL_TIMING){
            snprintf(text_scratch,256,"jpeg %04d color_ms=%d dct_quant_ms=%d entropy_ms=%d other_ms=%d",
                     num,(int)timing.ms[STREAM_JPEG_COLOR],(int)timing.ms[STREAM_JPEG_TRANSFORM],
                     (int)timing.ms[STREAM_JPEG_ENTROPY],(int)timing.ms[STREAM_ENCODE]);
        }else{
            snprintf(text_scratch,256,"jpeg %04d timing=OFF batch_mcus=%d",num,SE_JPEG_BATCH_MCUS);
        }
        log_line(text_scratch);
        snprintf(text_scratch,256,"look %04d grade_ms=%d mask_ms=%d glow_ms=%d grain_ms=%d other_ms=%d",
                 num,(int)look_timing.grade_ms,(int)look_timing.mask_ms,(int)look_timing.glow_ms,
                 (int)look_timing.grain_ms,(int)(timing.ms[STREAM_LOOK]-look_accounted));
        log_line(text_scratch);
        snprintf(text_scratch,256,"render %04d halation=%s grain=%s cache_requested=%s cache_used=%s cache_hits=%d cache_misses=%d",
                 num,halation_names[job_render.halation],job_render.grain?"STOCK":"OFF",
                 cache_names[job_render.cache],cache_names[cache],
                 (int)look_timing.cache_hits,(int)look_timing.cache_misses);
        log_line(text_scratch);
    }
    return success?1:skipped?0:-1;
}

static void job_settings(void)
{
    job_scale=set_res==0?1:2;job_mode=set_mode;
    job_delete=set_mode==M_NEWDEL;job_out=set_mode==M_MLSOUP;
    job_render=set_render;
}

static int ensure_output_dir(void)
{
    if(!is_dir("ML/SOUP"))FIO_CreateDirectory("ML/SOUP");
    return is_dir("ML/SOUP");
}

/* Queue up to MAX_JOB undeveloped frames after auto_base, up to auto_upto.
 * Runs in the worker so the clock callback never accesses the card. Returns
 * 0 for an empty queue or -1 if the card is unavailable and should be retried. */
static int auto_select(void)
{
    int i,n=0;
    ensure_loaded();
    if(!set_auto||!stock_count)return 0;
    job_stock=selected_stock;if(job_stock<0||job_stock>=stock_count)return 0;
    job_settings();
    if(index_build()!=0)return -1;
    if(auto_upto_folder>=100){
        const char *folder=strrchr(frames_dir,'/');
        if(!folder||dcim_folder_number(folder+1)!=auto_upto_folder)return -1;
    }
    for(i=0;i<nframes&&n<MAX_JOB;i++){
        frame_ent *e=&frames[i];
        if(e->num>auto_base&&e->num<=auto_upto&&!e->done&&!(e->flags&F_DUP))job_nums[n++]=e->num;
    }
    return n&&!ensure_output_dir()?-1:n;
}

static void job_task(void *unused)
{
    char text[16384],line[256];se_stock stock;int n,i,r,stopped=0,auto_batch_end=0;
    (void)unused;
    job_log_len=0;job_log[0]=0;
    if(job_auto){
        n=auto_select();
        if(n<=0){
            if(n==0)auto_base=auto_upto;
            if(!gui_task_alive)frames_release();
            job_auto=0;busy=0;return;
        }
        job_total=n;
        /* A full batch may have left more frames for the next clock tick. */
        auto_batch_end=n==MAX_JOB?job_nums[n-1]:auto_upto;
        /* Show the developing screen, as if Soup had been opened by hand: it
         * closes itself when done, or at once on a half-press. Not over Live
         * View, playback or a menu -- there the end-of-job note is enough.
         * lv reads 0 while a Live View shot is captured and reviewed, so
         * Live View in the last 10 s counts as Live View. */
        if(!lv&&lv_ago>=10&&!gui_task_alive&&!gui_menu_shown()&&!is_pure_play_photo_or_movie_mode()){
            gui_auto=1;if(gui_open()!=0)gui_auto=0;
        }
    }
    powersave_prohibit();                 /* no auto power-off mid-develop */
    snprintf(line,sizeof(line),"folder=%s stock=%s frames=%d scale=%d mode=%s%s",
        frames_dir,stock_names[job_stock],job_total,job_scale,mode_names[job_mode],job_auto?" auto":"");
    log_line(line);
    snprintf(line,sizeof(line),"mem malloc_free=%d am_free=%d",GetFreeMemForMalloc(),GetFreeMemForAllocateMemory());
    log_line(line);
    n=read_text(stock_paths[job_stock],text,sizeof(text));
    se_stock_init(&stock);
    if(n<=0||se_stock_parse(text,(size_t)n,&stock)!=SE_OK){
        snprintf(line,sizeof(line),"stock unreadable: %s rc=%d",stock_paths[job_stock],n);
        log_line(line);job_failed=job_total;if(job_auto)auto_base=auto_batch_end;goto out;}
    for(i=0;i<job_total;i++){
        if(JOB_STOP()){stopped=1;break;}
        job_pos=i;progress_pct=0;
        snprintf(status_text,sizeof(status_text),"Developing %d of %d",i+1,job_total);
        r=develop_frame(job_nums[i],&stock,line);
        if(r<0&&JOB_STOP()){stopped=1;break;}
        if(r>0)job_ok++;else if(r==0)job_skipped++;else job_failed++;
        /* auto: anything finished, skipped or failed is behind us (failures
         * are in FIND.TXT); a frame that was stopped is tried again */
        if(job_auto)auto_base=job_nums[i];
    }
    if(job_auto&&!stopped)auto_base=auto_batch_end;
out:
    write_small_file(FIND_LOG,job_log);
    diag_flush();
    powersave_permit();
    job_ms=get_ms_clock()-job_t0;
    job_result=job_ok?1:(cancel_job?-1:job_skipped&&!job_failed?-2:-1);
    snprintf(status_text,sizeof(status_text),"%d saved, %d failed",job_ok,job_failed);
    if(!ml_shutdown_requested&&!stop_worker&&!gui_running){
        if(!job_auto)NotifyBox(5000,"Soup: %d saved, %d failed, %d skipped",job_ok,job_failed,job_skipped);
        else if(job_ok||job_failed)NotifyBox(2000,"Soup: %d developed%s",job_ok,job_failed?", some failed":"");
    }
    job_auto=0;
    if(!gui_task_alive)frames_release();   /* GUI gone: nobody needs the index */
    busy=0;
}

/* Start developing `count` frames (index must be current). NULL = started. */
static const char *begin_job(const uint16_t *nums,int count)
{
    int i;
    if(busy||stop_worker||ml_shutdown_requested)return "BUSY";
    if(!stock_count)return "NO FILM STOCKS";
    if(count<1)return "NO PHOTOS TO DEVELOP";
    if(count>MAX_JOB)count=MAX_JOB;
    if(!ensure_output_dir())return "CANNOT CREATE ML/SOUP";
    job_stock=selected_stock;if(job_stock<0||job_stock>=stock_count)return "NO STOCK SELECTED";
    for(i=0;i<count;i++)job_nums[i]=nums[i];
    job_total=count;job_pos=0;job_ok=job_failed=job_skipped=job_deleted=job_kept=0;
    last_out[0]=0;last_from[0]=0;
    job_settings();
    cancel_job=0;progress_pct=0;job_result=0;job_t0=get_ms_clock();
    job_auto=0;busy=1;
    if(IS_ERROR(task_create("soup7d",0x1f,0x10000,job_task,0))){busy=0;return "TASK CREATION FAILED";}
    return 0;
}

/*
 * Auto develop, checked once a second from ML's clock task. Cheap and no
 * card I/O: it watches Canon's folder and file counters. Once a new shot has been
 * followed by two quiet seconds with the camera idle (capture job finished,
 * shutter not half-pressed, Soup screen closed), the job task is started in
 * auto mode and works out what to develop itself. The first shot after boot
 * also loads the settings; with AUTO off that is all that happens.
 */
static int auto_seen=-1, auto_seen_folder, auto_quiet;
static unsigned int soup_clock(unsigned int ctx)
{
    struct card_info *card=get_shooting_card();
    int fn,folder;
    (void)ctx;
    if(lv)lv_ago=0;else if(lv_ago<99)lv_ago++;
    if(!card||busy||stop_worker||ml_shutdown_requested)return CBR_RET_CONTINUE;
    fn=card->file_number;folder=card->folder_number;
    if(auto_seen<0){auto_base=auto_seen=fn;auto_seen_folder=folder;return CBR_RET_CONTINUE;}
    if(fn!=auto_seen||folder!=auto_seen_folder){
        if(fn<auto_seen||(folder!=auto_seen_folder&&fn==auto_seen))auto_base=fn-1;
        auto_seen=fn;auto_seen_folder=folder;auto_quiet=0;return CBR_RET_CONTINUE;
    }
    if(fn<1||fn<=auto_base||gui_task_alive)return CBR_RET_CONTINUE;
    if(loaded&&!set_auto){auto_base=fn;return CBR_RET_CONTINUE;}
    /* not while reviewing pictures or in ML's menu: a file must not be
     * swapped under Canon's playback while it is on screen */
    if(++auto_quiet<2||lens_info.job_state||get_halfshutter_pressed()||
       gui_menu_shown()||is_pure_play_photo_or_movie_mode())return CBR_RET_CONTINUE;
    auto_upto=fn;auto_upto_folder=folder;job_total=0;job_pos=0;job_ok=job_failed=job_skipped=job_deleted=job_kept=0;
    last_out[0]=0;last_from[0]=0;
    cancel_job=0;progress_pct=0;job_result=0;job_t0=get_ms_clock();
    job_auto=1;busy=1;
    if(IS_ERROR(task_create("soup7d",0x1f,0x10000,job_task,0))){job_auto=0;busy=0;}
    return CBR_RET_CONTINUE;
}
