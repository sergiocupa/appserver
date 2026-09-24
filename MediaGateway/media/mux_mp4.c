//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Muxer MP4 (ISO BMFF) embutido, video-only H.264 (amostras AVCC). Timescale=1000 (ms).
//  Progressivo (ftyp+mdat+moov) e Fragmentado (init + moof/mdat). NAO TESTADO EM RUNTIME.

#include "mux_mp4.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS 1000  // timescale (ms)

// ---- buffer + helpers de box ----
typedef struct { uint8_t* d; size_t len, cap; } Buf;
static void bneed(Buf* b, size_t n){ if(b->len+n<=b->cap) return; size_t c=b->cap?b->cap:4096; while(c<b->len+n)c*=2; b->d=(uint8_t*)memop_realloc_raw(b->d,c); b->cap=c; }
static void b8(Buf* b, uint8_t v){ bneed(b,1); b->d[b->len++]=v; }
static void bytes(Buf* b, const void* p, size_t n){ bneed(b,n); memcpy(b->d+b->len,p,n); b->len+=n; }
static void b16(Buf* b, uint32_t v){ b8(b,(v>>8)&0xFF); b8(b,v&0xFF); }
static void b32(Buf* b, uint32_t v){ b8(b,(v>>24)&0xFF); b8(b,(v>>16)&0xFF); b8(b,(v>>8)&0xFF); b8(b,v&0xFF); }
static void b64(Buf* b, uint64_t v){ b32(b,(uint32_t)(v>>32)); b32(b,(uint32_t)v); }
static void zero(Buf* b, int n){ for(int i=0;i<n;i++) b8(b,0); }
static void typ(Buf* b, const char* t){ bytes(b,t,4); }
// abre um box: escreve size placeholder + type; retorna a posicao do campo size.
static size_t box_begin(Buf* b, const char* t){ size_t p=b->len; b32(b,0); typ(b,t); return p; }
static void box_end(Buf* b, size_t p){ uint32_t sz=(uint32_t)(b->len-p); b->d[p]=(sz>>24)&0xFF; b->d[p+1]=(sz>>16)&0xFF; b->d[p+2]=(sz>>8)&0xFF; b->d[p+3]=sz&0xFF; }
static void patch32(Buf* b, size_t pos, uint32_t v){ b->d[pos]=(v>>24)&0xFF; b->d[pos+1]=(v>>16)&0xFF; b->d[pos+2]=(v>>8)&0xFF; b->d[pos+3]=v&0xFF; }

static const uint8_t MATRIX[36] = {0,1,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,1,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0x40,0,0,0};

static void wr_ftyp(Buf* b)
{
    size_t p=box_begin(b,"ftyp"); typ(b,"isom"); b32(b,0x200);
    typ(b,"isom"); typ(b,"iso2"); typ(b,"avc1"); typ(b,"hvc1"); typ(b,"mp41"); box_end(b,p);
}

// VisualSampleEntry: avc1+avcC (H.264) ou hvc1+hvcC (HEVC), conforme is_hevc.
static void wr_vse(Buf* b, int is_hevc, int w, int h, const uint8_t* cfg, int cfg_len)
{
    size_t p=box_begin(b, is_hevc?"hvc1":"avc1");
    zero(b,6); b16(b,1);                 // reserved + data_ref_index
    b16(b,0); b16(b,0); zero(b,12);      // predefined/reserved
    b16(b,w); b16(b,h);
    b32(b,0x00480000); b32(b,0x00480000);// h/v resolution 72dpi
    b32(b,0); b16(b,1);                  // reserved + frame_count
    zero(b,32);                          // compressorname
    b16(b,0x0018); b16(b,0xFFFF);        // depth + predefined
    { size_t c=box_begin(b, is_hevc?"hvcC":"avcC"); bytes(b,cfg,cfg_len); box_end(b,c); }
    box_end(b,p);
}

// stbl para PROGRESSIVO (com tabelas) ou FRAGMENTADO (tabelas vazias se count=0).
static void wr_stbl(Buf* b, int is_hevc, int w, int h, const uint8_t* cfg, int cfg_len,
                    const int64_t* pts, const int* size, const int* key, int count, uint64_t chunk_off)
{
    size_t p=box_begin(b,"stbl");
    { size_t s=box_begin(b,"stsd"); b32(b,0); b32(b,1); wr_vse(b,is_hevc,w,h,cfg,cfg_len); box_end(b,s); }

    // stts (RLE de duracoes em ms; ultima = penultima)
    { size_t s=box_begin(b,"stts"); b32(b,0);
      size_t cntpos=b->len; b32(b,0); uint32_t runs=0;
      if (count>0){
        int i=0;
        while(i<count){
            int dur = (i+1<count)? (int)(pts[i+1]-pts[i]) : (i>0? (int)(pts[i]-pts[i-1]) : (TS/30));
            if (dur<=0) dur = TS/30;
            int j=i+1;
            while(j<count){ int d2=(j+1<count)?(int)(pts[j+1]-pts[j]):dur; if(d2<=0)d2=dur; if(d2!=dur) break; j++; }
            b32(b,(uint32_t)(j-i)); b32(b,(uint32_t)dur); runs++; i=j;
        }
      }
      patch32(b,cntpos,runs); box_end(b,s);
    }
    // stss (sync samples)
    { size_t s=box_begin(b,"stss"); b32(b,0); size_t cp=b->len; b32(b,0); uint32_t n=0;
      for(int i=0;i<count;i++) if(key[i]){ b32(b,(uint32_t)(i+1)); n++; }
      patch32(b,cp,n); box_end(b,s);
    }
    // stsc: 1 chunk com todas as amostras
    { size_t s=box_begin(b,"stsc"); b32(b,0); b32(b, count>0?1:0); if(count>0){ b32(b,1); b32(b,(uint32_t)count); b32(b,1);} box_end(b,s); }
    // stsz
    { size_t s=box_begin(b,"stsz"); b32(b,0); b32(b,0); b32(b,(uint32_t)count); for(int i=0;i<count;i++) b32(b,(uint32_t)size[i]); box_end(b,s); }
    // stco
    { size_t s=box_begin(b,"stco"); b32(b,0); b32(b, count>0?1:0); if(count>0) b32(b,(uint32_t)chunk_off); box_end(b,s); }
    box_end(b,p);
}

static void wr_video_trak(Buf* b, int is_hevc, int w, int h, uint64_t duration, const uint8_t* cfg, int cfg_len,
                          const int64_t* pts, const int* size, const int* key, int count, uint64_t chunk_off)
{
    size_t p=box_begin(b,"trak");
    { size_t s=box_begin(b,"tkhd"); b8(b,0); b8(b,0);b8(b,0);b8(b,7); // v0, flags enabled|inmovie|inpreview
      b32(b,0); b32(b,0); b32(b,1); b32(b,0); b32(b,(uint32_t)duration);
      zero(b,8); b16(b,0); b16(b,0); b16(b,0); b16(b,0); bytes(b,MATRIX,36);
      b32(b,(uint32_t)w<<16); b32(b,(uint32_t)h<<16); box_end(b,s); }
    { size_t m=box_begin(b,"mdia");
      { size_t s=box_begin(b,"mdhd"); b32(b,0); b32(b,0); b32(b,0); b32(b,TS); b32(b,(uint32_t)duration); b16(b,0x55C4); b16(b,0); box_end(b,s); }
      { size_t s=box_begin(b,"hdlr"); b32(b,0); b32(b,0); typ(b,"vide"); zero(b,12); bytes(b,"VideoHandler",12); b8(b,0); box_end(b,s); }
      { size_t f=box_begin(b,"minf");
        { size_t s=box_begin(b,"vmhd"); b8(b,0);b8(b,0);b8(b,0);b8(b,1); b16(b,0); b16(b,0);b16(b,0);b16(b,0); box_end(b,s); }
        { size_t s=box_begin(b,"dinf"); { size_t dr=box_begin(b,"dref"); b32(b,0); b32(b,1); { size_t u=box_begin(b,"url "); b8(b,0);b8(b,0);b8(b,0);b8(b,1); box_end(b,u); } box_end(b,dr);} box_end(b,s); }
        wr_stbl(b,is_hevc,w,h,cfg,cfg_len,pts,size,key,count,chunk_off);
        box_end(b,f);
      }
      box_end(b,m);
    }
    box_end(b,p);
}

// ---- boxes de AUDIO (AAC / mp4a + esds) ----
// esds com AudioSpecificConfig (tamanhos de descritor em 1 byte; ASC pequeno).
static void wr_esds(Buf* b, const uint8_t* asc, int asc_len)
{
    size_t s=box_begin(b,"esds"); b32(b,0);                 // version/flags
    b8(b,0x03); b8(b,(uint8_t)(23+asc_len));                // ES_Descriptor
    b16(b,0); b8(b,0);                                      //   ES_ID(2)+flags(1)
    b8(b,0x04); b8(b,(uint8_t)(15+asc_len));                //   DecoderConfigDescriptor
    b8(b,0x40); b8(b,0x15);                                 //     objType=AAC(0x40), streamType=audio(0x15)
    b8(b,0); b16(b,0);                                      //     bufferSizeDB(3)=0
    b32(b,0); b32(b,0);                                     //     maxBitrate=0, avgBitrate=0
    b8(b,0x05); b8(b,(uint8_t)asc_len); bytes(b,asc,asc_len);//    DecoderSpecificInfo = ASC
    b8(b,0x06); b8(b,1); b8(b,0x02);                        //   SLConfigDescriptor
    box_end(b,s);
}

static void wr_mp4a(Buf* b, int channels, int rate, const uint8_t* asc, int asc_len)
{
    size_t p=box_begin(b,"mp4a");
    zero(b,6); b16(b,1);                    // reserved + data_ref_index
    b32(b,0); b32(b,0);                     // reserved(8)
    b16(b,channels); b16(b,16);             // channelcount + samplesize
    b16(b,0); b16(b,0);                     // predefined + reserved
    b32(b,(uint32_t)rate<<16);             // samplerate 16.16
    wr_esds(b,asc,asc_len);
    box_end(b,p);
}

// stbl de audio (tabelas se count>0; vazio p/ init fragmentado).
static void wr_audio_stbl(Buf* b, int rate, int channels, const uint8_t* asc, int asc_len,
                          const int64_t* dur, const int* size, int count, uint64_t chunk_off)
{
    size_t p=box_begin(b,"stbl");
    { size_t s=box_begin(b,"stsd"); b32(b,0); b32(b,1); wr_mp4a(b,channels,rate,asc,asc_len); box_end(b,s); }
    // stts (RLE das duracoes em unidades do timescale de audio)
    { size_t s=box_begin(b,"stts"); b32(b,0); size_t cp=b->len; b32(b,0); uint32_t runs=0;
      int i=0; while(i<count){ int d=(int)dur[i]; int j=i+1; while(j<count && (int)dur[j]==d) j++; b32(b,(uint32_t)(j-i)); b32(b,(uint32_t)d); runs++; i=j; }
      patch32(b,cp,runs); box_end(b,s);
    }
    // stsc: 1 chunk com todas as amostras
    { size_t s=box_begin(b,"stsc"); b32(b,0); b32(b,count>0?1:0); if(count>0){ b32(b,1); b32(b,(uint32_t)count); b32(b,1);} box_end(b,s); }
    // stsz
    { size_t s=box_begin(b,"stsz"); b32(b,0); b32(b,0); b32(b,(uint32_t)count); for(int i=0;i<count;i++) b32(b,(uint32_t)size[i]); box_end(b,s); }
    // stco
    { size_t s=box_begin(b,"stco"); b32(b,0); b32(b,count>0?1:0); if(count>0) b32(b,(uint32_t)chunk_off); box_end(b,s); }
    box_end(b,p);
}

static void wr_audio_trak(Buf* b, int rate, int channels, uint64_t movie_dur_ms, const uint8_t* asc, int asc_len,
                          const int64_t* dur, const int* size, int count, uint64_t chunk_off,
                          uint64_t media_dur_units, uint32_t track_id)
{
    size_t p=box_begin(b,"trak");
    { size_t s=box_begin(b,"tkhd"); b8(b,0); b8(b,0);b8(b,0);b8(b,7);
      b32(b,0); b32(b,0); b32(b,track_id); b32(b,0); b32(b,(uint32_t)movie_dur_ms);
      zero(b,8); b16(b,0); b16(b,0); b16(b,0x0100); b16(b,0); bytes(b,MATRIX,36);  // volume=1.0
      b32(b,0); b32(b,0); box_end(b,s); }                                          // width/height = 0
    { size_t m=box_begin(b,"mdia");
      { size_t s=box_begin(b,"mdhd"); b32(b,0); b32(b,0); b32(b,0); b32(b,(uint32_t)rate); b32(b,(uint32_t)media_dur_units); b16(b,0x55C4); b16(b,0); box_end(b,s); }
      { size_t s=box_begin(b,"hdlr"); b32(b,0); b32(b,0); typ(b,"soun"); zero(b,12); bytes(b,"SoundHandler",12); b8(b,0); box_end(b,s); }
      { size_t f=box_begin(b,"minf");
        { size_t s=box_begin(b,"smhd"); b32(b,0); b16(b,0); b16(b,0); box_end(b,s); }
        { size_t s=box_begin(b,"dinf"); { size_t dr=box_begin(b,"dref"); b32(b,0); b32(b,1); { size_t u=box_begin(b,"url "); b8(b,0);b8(b,0);b8(b,0);b8(b,1); box_end(b,u); } box_end(b,dr);} box_end(b,s); }
        wr_audio_stbl(b,rate,channels,asc,asc_len,dur,size,count,chunk_off);
        box_end(b,f);
      }
      box_end(b,m);
    }
    box_end(b,p);
}

// ---------------- Progressivo ----------------
struct Mp4Mux
{
    FILE* f; int w,h,is_hevc; uint8_t* avcc; int avcc_len;   // avcc = avcC (H.264) ou hvcC (HEVC)
    size_t mdat_size_pos; uint64_t first_sample_off;
    int64_t* pts; int* size; int* key; int count, cap;
    // audio (AAC passthrough, opcional)
    int has_audio, a_rate, a_channels, asc_len;
    uint8_t* asc;
    int64_t* a_dur; int* a_size; int a_count, a_cap;
    uint8_t* a_data; size_t a_len, a_cap_bytes;   // bytes de audio bufferizados
};

Mp4Mux* mp4_open_full(const char* path, int is_hevc, int width, int height, const uint8_t* cfg, int cfg_len,
                      const uint8_t* asc, int asc_len, int rate, int channels)
{
    Mp4Mux* m=(Mp4Mux*)memop_calloc_raw(1,sizeof(Mp4Mux)); if(!m) return 0;
    if (fopen_s(&m->f,path,"wb")!=0 || !m->f){ memop_free_raw(m); return 0; }
    m->w=width; m->h=height; m->is_hevc=is_hevc; m->avcc=(uint8_t*)memop_alloc_raw(cfg_len); memcpy(m->avcc,cfg,cfg_len); m->avcc_len=cfg_len;
    if (asc && asc_len>0 && rate>0)
    {
        m->has_audio=1; m->a_rate=rate; m->a_channels=channels>0?channels:2;
        m->asc=(uint8_t*)memop_alloc_raw(asc_len); memcpy(m->asc,asc,asc_len); m->asc_len=asc_len;
    }

    Buf b={0}; wr_ftyp(&b); fwrite(b.d,1,b.len,m->f); memop_free_raw(b.d);
    m->mdat_size_pos=ftell(m->f);
    { uint8_t hdr[8]={0,0,0,0,'m','d','a','t'}; fwrite(hdr,1,8,m->f); }
    m->first_sample_off=ftell(m->f);
    return m;
}

Mp4Mux* mp4_open2(const char* path, int width, int height, const uint8_t* avcc, int avcc_len,
                  const uint8_t* asc, int asc_len, int rate, int channels)
{
    return mp4_open_full(path,0,width,height,avcc,avcc_len,asc,asc_len,rate,channels);
}

Mp4Mux* mp4_open(const char* path, int width, int height, const uint8_t* avcc, int avcc_len)
{
    return mp4_open_full(path,0,width,height,avcc,avcc_len,0,0,0,0);
}

Mp4Mux* mp4_open_hevc(const char* path, int width, int height, const uint8_t* hvcc, int hvcc_len,
                      const uint8_t* asc, int asc_len, int rate, int channels)
{
    return mp4_open_full(path,1,width,height,hvcc,hvcc_len,asc,asc_len,rate,channels);
}

void mp4_write_audio(Mp4Mux* m, int dur_units, const uint8_t* s, int size)
{
    if(!m||!m->has_audio||!s||size<=0) return;
    if(m->a_count==m->a_cap){ m->a_cap=m->a_cap?m->a_cap*2:1024; m->a_dur=(int64_t*)memop_realloc_raw(m->a_dur,m->a_cap*sizeof(int64_t)); m->a_size=(int*)memop_realloc_raw(m->a_size,m->a_cap*sizeof(int)); }
    m->a_dur[m->a_count]=dur_units>0?dur_units:1024; m->a_size[m->a_count]=size; m->a_count++;
    if(m->a_len+size>m->a_cap_bytes){ size_t c=m->a_cap_bytes?m->a_cap_bytes:65536; while(c<m->a_len+size)c*=2; m->a_data=(uint8_t*)memop_realloc_raw(m->a_data,c); m->a_cap_bytes=c; }
    memcpy(m->a_data+m->a_len,s,size); m->a_len+=size;
}

void mp4_write_video(Mp4Mux* m, int64_t pts_ms, int keyframe, const uint8_t* s, int size)
{
    if(!m||!s||size<=0) return;
    if(m->count==m->cap){ m->cap=m->cap?m->cap*2:1024; m->pts=(int64_t*)memop_realloc_raw(m->pts,m->cap*sizeof(int64_t)); m->size=(int*)memop_realloc_raw(m->size,m->cap*sizeof(int)); m->key=(int*)memop_realloc_raw(m->key,m->cap*sizeof(int)); }
    m->pts[m->count]=pts_ms; m->size[m->count]=size; m->key[m->count]=keyframe?1:0; m->count++;
    fwrite(s,1,size,m->f);
}

void mp4_close(Mp4Mux* m)
{
    if(!m) return;

    // Audio (se houver) vai como um 2o chunk no mesmo mdat, apos todo o video.
    uint64_t a_off = (uint64_t)ftell(m->f);
    if (m->has_audio && m->a_len) fwrite(m->a_data,1,m->a_len,m->f);

    long end=ftell(m->f);
    // patch mdat size (cobre video + audio)
    uint32_t mdat_size=(uint32_t)(end - m->mdat_size_pos);
    fseek(m->f,(long)m->mdat_size_pos,SEEK_SET);
    { uint8_t sz[4]={ (mdat_size>>24)&0xFF,(mdat_size>>16)&0xFF,(mdat_size>>8)&0xFF,mdat_size&0xFF }; fwrite(sz,1,4,m->f); }
    fseek(m->f,0,SEEK_END);

    uint64_t vdur = m->count>0 ? (uint64_t)(m->pts[m->count-1] + (m->count>1? (m->pts[m->count-1]-m->pts[m->count-2]) : (TS/30))) : 0;

    // duracao do filme (timescale=1000) = max(video, audio em ms)
    uint64_t a_units=0; for(int i=0;i<m->a_count;i++) a_units+=(uint64_t)m->a_dur[i];
    uint64_t adur_ms = (m->has_audio && m->a_rate>0) ? (a_units*1000/(uint64_t)m->a_rate) : 0;
    uint64_t mv_dur = vdur>adur_ms?vdur:adur_ms;
    uint32_t next_track = m->has_audio && m->a_count>0 ? 3 : 2;

    Buf b={0}; size_t p=box_begin(&b,"moov");
    { size_t s=box_begin(&b,"mvhd"); b32(&b,0); b32(&b,0); b32(&b,0); b32(&b,TS); b32(&b,(uint32_t)mv_dur);
      b32(&b,0x00010000); b16(&b,0x0100); b16(&b,0); b32(&b,0); b32(&b,0); bytes(&b,MATRIX,36); zero(&b,24); b32(&b,next_track); box_end(&b,s); }
    wr_video_trak(&b, m->is_hevc, m->w, m->h, vdur, m->avcc, m->avcc_len, m->pts, m->size, m->key, m->count, m->first_sample_off);
    if (m->has_audio && m->a_count>0)
        wr_audio_trak(&b, m->a_rate, m->a_channels, adur_ms, m->asc, m->asc_len,
                      m->a_dur, m->a_size, m->a_count, a_off, a_units, 2);
    box_end(&b,p);
    fwrite(b.d,1,b.len,m->f); memop_free_raw(b.d);

    fclose(m->f); memop_free_raw(m->avcc); memop_free_raw(m->pts); memop_free_raw(m->size); memop_free_raw(m->key);
    memop_free_raw(m->asc); memop_free_raw(m->a_dur); memop_free_raw(m->a_size); memop_free_raw(m->a_data); memop_free_raw(m);
}

// ---------------- Fragmentado (CMAF) ----------------
// Os dois build_* montam em MEMORIA; os write_* sao eles mais um fwrite. O ao vivo de
// camera serve init/segmento direto da RAM (nunca toca o disco), e o VOD continua
// gravando -- um so lugar monta as caixas.
int mp4_build_init(uint8_t** out, int* out_len, int width, int height, const uint8_t* avcc, int avcc_len)
{
    if (!out || !out_len) return -1;
    Buf b={0}; wr_ftyp(&b);
    size_t p=box_begin(&b,"moov");
    { size_t s=box_begin(&b,"mvhd"); b32(&b,0); b32(&b,0); b32(&b,0); b32(&b,TS); b32(&b,0);
      b32(&b,0x00010000); b16(&b,0x0100); b16(&b,0); b32(&b,0); b32(&b,0); bytes(&b,MATRIX,36); zero(&b,24); b32(&b,2); box_end(&b,s); }
    wr_video_trak(&b, 0, width, height, 0, avcc, avcc_len, 0, 0, 0, 0, 0);  // stbl vazio (H.264)
    { size_t mx=box_begin(&b,"mvex");
      { size_t tx=box_begin(&b,"trex"); b32(&b,0); b32(&b,1); b32(&b,1); b32(&b,0); b32(&b,0); b32(&b,0); box_end(&b,tx); }
      box_end(&b,mx);
    }
    box_end(&b,p);
    *out = b.d; *out_len = (int)b.len;
    return 0;
}

int mp4_write_init(const char* path, int width, int height, const uint8_t* avcc, int avcc_len)
{
    uint8_t* data=0; int len=0;
    if (mp4_build_init(&data,&len,width,height,avcc,avcc_len)!=0 || !data) return -1;
    FILE* f=0; if (fopen_s(&f,path,"wb")!=0 || !f) { memop_free_raw(data); return -1; }
    fwrite(data,1,(size_t)len,f); fclose(f); memop_free_raw(data);
    return 0;
}

int mp4_build_segment(uint8_t** out, int* out_len, uint32_t seq, int64_t base_dt_ms, const Mp4Sample* samples, int count)
{
    if(count<=0 || !out || !out_len) return -1;

    Buf mf={0}; size_t moof=box_begin(&mf,"moof");
    { size_t s=box_begin(&mf,"mfhd"); b32(&mf,0); b32(&mf,seq); box_end(&mf,s); }
    size_t data_off_pos=0;
    { size_t tf=box_begin(&mf,"traf");
      { size_t s=box_begin(&mf,"tfhd"); b8(&mf,0); b8(&mf,0x02);b8(&mf,0x00);b8(&mf,0x00); b32(&mf,1); box_end(&mf,s); } // default-base-is-moof
      { size_t s=box_begin(&mf,"tfdt"); b8(&mf,1); b8(&mf,0);b8(&mf,0);b8(&mf,0); b64(&mf,(uint64_t)base_dt_ms); box_end(&mf,s); } // v1
      { size_t s=box_begin(&mf,"trun");
        // version(1)=0 + flags(3)=0x000701:
        //   data-offset(0x000001) + sample-duration(0x000100) + sample-size(0x000200) + sample-flags(0x000400)
        b8(&mf,0); b8(&mf,0x00); b8(&mf,0x07); b8(&mf,0x01);
        b32(&mf,(uint32_t)count);
        data_off_pos=mf.len; b32(&mf,0); // data_offset (patch depois)
        for(int i=0;i<count;i++){
            b32(&mf,(uint32_t)(samples[i].duration_ms>0?samples[i].duration_ms:(TS/30)));
            b32(&mf,(uint32_t)samples[i].size);
            b32(&mf, samples[i].keyframe ? 0x02000000u : 0x01010000u); // sample_flags (sync / non-sync)
        }
        box_end(&mf,s);
      }
      box_end(&mf,tf);
    }
    box_end(&mf,moof);

    uint32_t data_offset=(uint32_t)(mf.len + 8); // moof inteiro + header do mdat
    patch32(&mf, data_off_pos, data_offset);

    // mdat, no MESMO buffer do moof
    uint32_t total=8; for(int i=0;i<count;i++) total+=samples[i].size;
    { uint8_t hdr[8]={ (uint8_t)((total>>24)&0xFF),(uint8_t)((total>>16)&0xFF),(uint8_t)((total>>8)&0xFF),(uint8_t)(total&0xFF),'m','d','a','t' };
      bytes(&mf,hdr,8); }
    for(int i=0;i<count;i++) bytes(&mf,samples[i].data,(size_t)samples[i].size);

    *out = mf.d; *out_len = (int)mf.len;
    return 0;
}

int mp4_write_segment(const char* path, uint32_t seq, int64_t base_dt_ms, const Mp4Sample* samples, int count)
{
    uint8_t* data=0; int len=0;
    if (mp4_build_segment(&data,&len,seq,base_dt_ms,samples,count)!=0 || !data) return -1;
    FILE* f=0; if (fopen_s(&f,path,"wb")!=0 || !f) { memop_free_raw(data); return -1; }
    fwrite(data,1,(size_t)len,f); fclose(f); memop_free_raw(data);
    return 0;
}

// ---------------- Fragmentado: init de AUDIO (AAC) ----------------
int mp4_write_init_audio(const char* path, const uint8_t* asc, int asc_len, int rate, int channels)
{
    if (!asc || asc_len<=0 || rate<=0) return -1;
    FILE* f=0; if (fopen_s(&f,path,"wb")!=0 || !f) return -1;
    Buf b={0}; wr_ftyp(&b);
    size_t p=box_begin(&b,"moov");
    { size_t s=box_begin(&b,"mvhd"); b32(&b,0); b32(&b,0); b32(&b,0); b32(&b,TS); b32(&b,0);
      b32(&b,0x00010000); b16(&b,0x0100); b16(&b,0); b32(&b,0); b32(&b,0); bytes(&b,MATRIX,36); zero(&b,24); b32(&b,2); box_end(&b,s); }
    wr_audio_trak(&b, rate, channels>0?channels:2, 0, asc, asc_len, 0, 0, 0, 0, 0, 1);  // stbl vazio
    { size_t mx=box_begin(&b,"mvex");
      { size_t tx=box_begin(&b,"trex"); b32(&b,0); b32(&b,1); b32(&b,1); b32(&b,0); b32(&b,0); b32(&b,0); box_end(&b,tx); }
      box_end(&b,mx);
    }
    box_end(&b,p);
    fwrite(b.d,1,b.len,f); memop_free_raw(b.d); fclose(f);
    return 0;
}
