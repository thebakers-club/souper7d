/* Host test: next-free output numbering, EXIF "#src" lookup, self-heal
 * rename when Canon reuses one of our numbers, in-place develop recovery
 * (SOUI####.TMP) and in-place "developed" detection. Includes production part_frames.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#define FIO_MAX_PATH_LENGTH 0x80
#define ATTR_DIRECTORY 0x10
#define IS_ERROR(p) ((uintptr_t)(p) & 1)
struct fio_dirent { int i; const char **l; };
struct fio_file { uint32_t mode,size,timestamp,off_0x0c; char name[FIO_MAX_PATH_LENGTH]; uint32_t a,b,c,d; };
struct card_info { int file_number; };
static volatile int stop_worker,busy,gui_running; static int ml_shutdown_requested;
static const char *dcim_list[64], *soup_list[16]; static int open_dirs; static struct card_info card;
static struct fio_dirent dirs[4];
static const char *test_dir="A:/DCIM/100EOS7D";
static const char *get_dcim_dir(void){ return test_dir; }
static struct card_info *get_shooting_card(void){ return &card; }
/* directory listing comes from the memfile table so renames show up */
#include "ml-file-stubs.h"
static const char *names_d[64], *names_s[64];
static void relist(void){
  int a=0,b=0;size_t len=strlen(test_dir);
  for(int i=0;i<nmfs;i++){
    const char *p=mfs[i].path;
    if(!strncmp(p,test_dir,len)&&p[len]=='/') names_d[a++]=p+len+1;
    else if(!strncmp(p,"ML/SOUP/",8)) names_s[b++]=p+8;
  }
  names_d[a]=0; names_s[b]=0;
}
static int fill(struct fio_dirent *d,struct fio_file *f){ if(!d->l[d->i])return -1; memset(f,0,sizeof *f);
  snprintf(f->name,sizeof f->name,"%s",d->l[d->i++]); return 0; }
static struct fio_dirent *FIO_FindFirstEx(const char *dir,struct fio_file *f){
  relist();
  const char **l=!strcmp(dir,test_dir)?names_d:!strcmp(dir,"ML/SOUP")?names_s:0;
  if(!l)return (struct fio_dirent*)1; struct fio_dirent *d=&dirs[open_dirs++]; d->i=0; d->l=l;
  if(fill(d,f))memset(f,0,sizeof *f); return d; }
static int FIO_FindNextEx(struct fio_dirent *d,struct fio_file *f){ return fill(d,f); }
static void FIO_FindClose(struct fio_dirent *d){ (void)d; open_dirs--; }
#include "../parts/part_frames.c"

#define D "A:/DCIM/100EOS7D/"
static int done(int n){ int i=frame_lookup(n); return i>=0?frames[i].done:-1; }
int main(void){
  char o[96],t[96],tag[96],tag_second[96]; int n;
  /* Canon frames 0525..0530 (+ old _MG_0001); legacy output SOUP0526 (no #);
   * new-style SOUP0531 made from 0527; ML/SOUP output SOUP0003 made from 0529 */
  mf_add(D"_MG_0001.CR2",0);
  mf_add(D"_B7D0525.CR2",0); mf_add(D"_B7D0525.JPG",0);
  mf_add(D"_B7D0526.JPG",0); mf_add(D"SOUP0526.JPG","xxSOUP ENGINE - GOLD\0yy");
  mf_add(D"_B7D0527.JPG",0); mf_add(D"_B7D0528.JPG",0); mf_add(D"_B7D0529.CR2",0); mf_add(D"_B7D0530.JPG",0);
  mf_add(D"SOUP0531.JPG","\xff\xd8 exif... SOUP ENGINE - PORTRA 160 #0527\0 more");
  mf_add("ML/SOUP/SOUP0003.JPG","SOUP ENGINE @0F4451/100EOS7D/_B7D - HP5 PLUS #0529");
  card.file_number=530;
  assert(index_build()==0&&open_dirs==0);
  assert(nframes==7);                              /* outputs are not frames */
  assert(done(526)==1&&done(527)==1&&done(529)==1&&done(528)==0&&done(530)==0);
  assert(max_cam==531&&max_ml==3);
  n=next_output(0,o,t,sizeof o); assert(n==532&&!strcmp(o,D"SOUP0532.JPG")&&!strcmp(t,D"SOUP0532.TMP"));
  n=next_output(0,o,t,sizeof o); assert(n==533);   /* batch: consecutive numbers */
  n=next_output(1,o,t,sizeof o); assert(n==4&&!strcmp(o,"ML/SOUP/SOUP0004.JPG"));
  /* Canon counter ahead of the folder (shots deleted): stay above the counter */
  card.file_number=600; assert(index_build()==0);
  n=next_output(0,o,t,sizeof o); assert(n==601);
  /* a .TMP left behind blocks that number */
  card.file_number=530; mf_add(D"SOUP0532.TMP",0); assert(index_build()==0);
  n=next_output(0,o,t,sizeof o); assert(n==533);
  /* SELF-HEAL: camera shot 0531 before it knew about our SOUP0531 */
  mf_add(D"_B7D0531.CR2",0); mf_add(D"_B7D0531.JPG",0); card.file_number=531; renames=0;
  assert(index_build()==0);
  assert(renames==1&&!is_file(D"SOUP0531.JPG")&&is_file(D"SOUP0533.JPG"));  /* 0532 blocked by .TMP */
  assert(is_file(D"_B7D0531.JPG")&&is_file(D"_B7D0531.CR2"));                 /* camera files untouched */
  assert(done(527)==1&&done(531)==0);              /* still credited to its source, not to the new shot */
  /* a second scan is stable: nothing more to rename */
  renames=0; assert(index_build()==0&&renames==0&&done(527)==1);
  /* legacy output sharing its own source's number is NOT renamed */
  assert(is_file(D"SOUP0526.JPG")&&done(526)==1);
  /* IN PLACE, interrupted after Canon's JPG was removed: the TMP becomes
   * the frame's JPG under the CR2's prefix */
  mf_add(D"_B7D0540.CR2",0); mf_add(D"SOUI0540.TMP","\xff\xd8 SOUP ENGINE - GOLD #0540 ........................................... \xff\xd9");
  /* a RAW-only develop cut off mid-write: no EOI, so it is discarded */
  mf_add(D"_B7D0539.CR2",0); mf_add(D"SOUI0539.TMP","\xff\xd8 SOUP ENGINE - GOLD #0539 cut off .......................................");
  /* IN PLACE, interrupted before the swap: the JPG is still the original */
  mf_add(D"_B7D0541.JPG","\xff\xd8 canon"); mf_add(D"SOUI0541.TMP","\xff\xd8 half-written");
  card.file_number=541; renames=0; removes=0;
  assert(index_build()==0);
  assert(!is_file(D"SOUI0540.TMP")&&is_file(D"_B7D0540.JPG")&&is_file(D"_B7D0540.CR2")&&done(540)==1);
  assert(!is_file(D"SOUI0541.TMP")&&is_file(D"_B7D0541.JPG")&&done(541)==0);
  assert(!is_file(D"SOUI0539.TMP")&&!is_file(D"_B7D0539.JPG")&&is_file(D"_B7D0539.CR2")&&done(539)==0);
  assert(renames==1&&removes==2);
  /* developed in place: the newest tagged JPG marks where ALL NEW starts */
  mf_add(D"_B7D0545.JPG","\xff\xd8 exif SOUP ENGINE - T-MAX 400 #0545");
  mf_add(D"_B7D0546.JPG","\xff\xd8 canon"); mf_add(D"_B7D0547.CR2",0);
  card.file_number=547; assert(index_build()==0);
  assert(done(545)==1&&done(546)==0&&done(547)==0);
  { uint16_t sel[8]; int k=select_new(sel,8); assert(k==2&&sel[0]==546&&sel[1]==547); }
  /* the tag check itself */
  { uint8_t b[64]; assert(soup_marked(D"_B7D0545.JPG",b,64)==1&&soup_marked(D"_B7D0546.JPG",b,64)==0); }
  /* 9999 reached */
  card.file_number=9999; assert(index_build()==0); assert(next_output(0,o,t,sizeof o)==-1);
  /* Old flat-folder outputs cannot prove their directory. New tags must
   * match the entire folder name and one of the source file prefixes. */
  mf_add("ML/SOUP/SOUP0004.JPG","SOUP ENGINE - GOLD #0529");
  mf_add("A:/DCIM/101EOS7D/_B7D0529.CR2",0);
  mf_add("A:/DCIM/101EOS7D/_B7D0530.JPG",0);
  test_dir="A:/DCIM/101EOS7D";card.file_number=530;
  assert(index_build()==0&&done(529)==0);
  { uint16_t sel[2]; assert(select_new(sel,2)==1&&sel[0]==530); }
  format_source_tag(tag,sizeof tag,"GOLD","_B7D",529);
  mf_add("ML/SOUP/SOUP0005.JPG",tag);
  assert(index_build()==0&&done(529)==1&&done(530)==0);
  test_dir="A:/DCIM/101OTHER";mf_add("A:/DCIM/101OTHER/_B7D0529.CR2",0);
  assert(index_build()==0&&done(529)==0);
  format_source_tag(tag_second,sizeof tag_second,"GOLD","IMG_",529);
  mf_add("ML/SOUP/SOUP0006.JPG",tag_second);
  assert(index_build()==0&&done(529)==0);
  /* A malformed qualified tag cannot fall back to its legacy number. */
  mf_add("A:/DCIM/101OTHER/SOUP0529.JPG","SOUP ENGINE @FFFFFF/101OTHER/_B7D - GOLD #0529");
  assert(index_build()==0&&done(529)==0);
  test_dir="A:/DCIM/999EOS7D";
  assert(index_build()==0);
  format_source_tag(tag,sizeof tag,"GOLD","IMG_",9999);
  assert(!strcmp(tag,"SOUP ENGINE @98967F/999EOS7D/IMG_ - GOLD #9999"));
  frames_release();
  puts("numbering-regression: all checks passed"); return 0;
}
