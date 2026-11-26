/* Run with the path to an existing, empty directory as a mock CF card. */
#include <assert.h>
#include "mlstub/module.h"

char STUB_ROOT[256];
struct lens_info_stub lens_info;
struct card_info stub_card;
int ml_shutdown_requested,lv,menu_redraw_blocked,stub_half,stub_half_after;

#include "../parts/head.c"
#include "../parts/part_frames.c"
#include "../parts/part_meta.c"
#include "../parts/part_job.c"

static void ensure_loaded(void)
{
    if(loaded)return;
    loaded=1;
    load_stocks();
}

static int gui_open(void){return -1;}

static int write_jpeg(void *file,const uint8_t *data,size_t size)
{
    return fwrite(data,1,size,file)==size?0:-1;
}

static void add_frame(int num)
{
    enum {WIDTH=64,HEIGHT=48};
    char path[64];
    uint8_t strip[8*WIDTH*3],row[WIDTH*3];
    se_jpeg enc;
    memset(row,100,sizeof row);
    snprintf(path,sizeof path,"%s/IMG_%04d.JPG",get_dcim_dir(),num);
    FILE *f=FIO_CreateFile(path);
    assert(f);
    assert(se_jpeg_begin(&enc,WIDTH,HEIGHT,88,strip,0,write_jpeg,f)==SE_OK);
    for(int y=0;y<HEIGHT;y++)assert(se_jpeg_write_rows(&enc,row,1)==SE_OK);
    assert(se_jpeg_end(&enc)==SE_OK);
    FIO_CloseFile(f);
}

int main(int argc,char **argv)
{
    assert(argc==2&&strlen(argv[1])<sizeof STUB_ROOT);
    strcpy(STUB_ROOT,argv[1]);
    assert(FIO_CreateDirectory("ML")==0);
    assert(FIO_CreateDirectory("ML/FILM")==0);
    write_small_file("ML/FILM/INDEX.TXT","test.fs\n");
    write_small_file("ML/FILM/test.fs","VERSION 3\nNAME TEST\n");
    set_auto=1;
    set_mode=M_MLSOUP;
    lv=1;
    stub_card.folder_number=100;

    soup_clock(0);
    stub_card.file_number=MAX_JOB+1;
    soup_clock(0);
    soup_clock(0);
    soup_clock(0);
    assert(loaded&&stock_count==1);
    assert(auto_base==0&&!busy);  /* Missing DCIM must not consume the backlog. */

    assert(FIO_CreateDirectory("DCIM")==0);
    assert(FIO_CreateDirectory("DCIM/100EOS7D")==0);
    for(int i=1;i<=MAX_JOB+1;i++)add_frame(i);
    assert(!is_dir("ML/SOUP"));

    soup_clock(0);
    assert(job_total==MAX_JOB&&job_ok==MAX_JOB&&auto_base==MAX_JOB);
    assert(is_dir("ML/SOUP")&&is_file(FIND_LOG));
    assert(!is_file("ML/SOUP/SOUP0201.JPG"));
    soup_clock(0);
    assert(job_total==1&&job_ok==1&&auto_base==MAX_JOB+1);
    assert(is_file("ML/SOUP/SOUP0201.JPG"));
    assert(is_file("A:/DCIM/100EOS7D/IMG_0201.JPG"));

    /* Switching folders with the same counter must trigger a new develop. */
    assert(FIO_CreateDirectory("DCIM/101EOS7D")==0);
    stub_card.folder_number=101;
    add_frame(MAX_JOB+1);
    soup_clock(0);soup_clock(0);soup_clock(0);
    assert(job_total==1&&job_ok==1&&is_file("ML/SOUP/SOUP0202.JPG"));
    assert(index_build()==0&&frames[frame_lookup(MAX_JOB+1)].done);
    frames_release();
    /* A reset to zero on an empty new folder must not consume frame 0001. */
    assert(FIO_CreateDirectory("DCIM/102EOS7D")==0);
    stub_card.folder_number=102;
    stub_card.file_number=0;
    soup_clock(0);
    add_frame(1);
    stub_card.file_number=1;
    soup_clock(0);soup_clock(0);soup_clock(0);
    assert(job_total==1&&job_ok==1&&is_file("ML/SOUP/SOUP0203.JPG"));
    /* Continuous numbering after creating a folder follows the next shot. */
    assert(FIO_CreateDirectory("DCIM/103EOS7D")==0);
    stub_card.folder_number=103;
    stub_card.file_number=2;
    add_frame(2);
    soup_clock(0);soup_clock(0);soup_clock(0);
    assert(job_total==1&&job_ok==1&&is_file("ML/SOUP/SOUP0204.JPG"));
    puts("auto-backlog-regression: 201-frame backlog and three folder transitions passed");
    return 0;
}
