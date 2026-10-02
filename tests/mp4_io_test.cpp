// Exercise the selected real SDK format engine, not a mock MP4 API.
#include "lawrec_mp4_io.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

enum Fault { NONE, WRITE, SHORT_WRITE, SEEK, FLUSH, CLOSE, TELL, READ };
static Fault fault;
static unsigned failures;
static bool observe_instance;
static void *instance_allocation;
static unsigned fail_writer_allocation;
extern "C" {
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
size_t __real_fread(void *, size_t, size_t, FILE *);
int __real_fseek(FILE *, long, int);
int __real_fflush(FILE *);
int __real_fclose(FILE *);
long __real_ftell(FILE *);
void *__real_calloc(size_t, size_t);
void __real_free(void *);

void *__wrap_calloc(size_t count, size_t size)
{
    // Fail the inline wrapper/engine allocation, after the large MP4 instance.
    if (fail_writer_allocation && count==1 && size<4*1024*1024 && !--fail_writer_allocation) {
        errno=ENOMEM; return nullptr;
    }
    void *value=__real_calloc(count,size);
    if (observe_instance && count==1 && size>=4*1024*1024 && value) {
        assert(!instance_allocation); instance_allocation=value;
    }
    return value;
}
void __wrap_free(void *value)
{
    if (value==instance_allocation) instance_allocation=nullptr;
    __real_free(value);
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (fault==WRITE) { ++failures; errno=ENOSPC; return 0; }
    if (fault==SHORT_WRITE && count>1) {
        ++failures;
        size_t result=__real_fwrite(data,size,count-1,file);
        errno=0; // No errno/ferror: a short write still must not look successful.
        return result;
    }
    return __real_fwrite(data,size,count,file);
}
size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (fault==READ) { ++failures; errno=EIO; return 0; }
    return __real_fread(data,size,count,file);
}
int __wrap_fseek(FILE *file, long offset, int whence)
{
    if (fault==SEEK) { ++failures; errno=EIO; return -1; }
    return __real_fseek(file,offset,whence);
}
int __wrap_fflush(FILE *file)
{
    if (fault==FLUSH) { ++failures; errno=ENOSPC; return EOF; }
    return __real_fflush(file);
}
int __wrap_fclose(FILE *file)
{
    int result=__real_fclose(file); // Even a failed close consumes the FILE handle.
    if (fault==CLOSE) { ++failures; errno=EIO; return EOF; }
    return result;
}
long __wrap_ftell(FILE *file)
{
    if (fault==TELL) { ++failures; errno=EIO; return -1; }
    return __real_ftell(file);
}
}

static unsigned char nal[] = {
    0,0,0,1,0x67,0x42,0xe0,0x1e,0xab,0xcd,
    0,0,0,1,0x68,0xce,0x3c,0x80,
    0,0,0,1,0x65,0x80,0x01
};
static k_mp4_frame_data_s sample()
{
    k_mp4_frame_data_s frame{};
    frame.codec_id=K_MP4_CODEC_ID_H264; frame.data=nal; frame.data_length=sizeof(nal);
    return frame;
}
static KD_HANDLE open_video(const std::string &path, KD_HANDLE &track)
{
    fault=NONE;
    k_mp4_config_s config{};
    config.config_type=K_MP4_CONFIG_MUXER;
    assert(path.size()<sizeof(config.muxer_config.file_name));
    strcpy(config.muxer_config.file_name,path.c_str());
    KD_HANDLE handle=nullptr;
    assert(!kd_mp4_create(&handle,&config));
    k_mp4_track_info_s info{};
    info.track_type=K_MP4_STREAM_VIDEO; info.time_scale=1000;
    info.video_info={1280,720,1,K_MP4_CODEC_ID_H264};
    assert(!kd_mp4_create_track(handle,&track,&info));
    return handle;
}
static void close_video(KD_HANDLE handle, int expected)
{
    assert(!kd_mp4_destroy_tracks(handle));
    int actual=kd_mp4_destroy(handle);
    if (actual!=expected) fprintf(stderr,"close fault=%d expected=%d actual=%d\n",fault,expected,actual);
    assert(actual==expected);
    fault=NONE;
}

struct Box { size_t start, end; std::string type; };
static uint32_t be32(const std::vector<uint8_t> &data, size_t position)
{
    assert(position+4 <= data.size());
    return uint32_t(data[position])<<24 | uint32_t(data[position+1])<<16 |
           uint32_t(data[position+2])<<8 | data[position+3];
}
static std::vector<Box> boxes(const std::vector<uint8_t> &data, size_t start, size_t end)
{
    std::vector<Box> result;
    for (size_t position=start; position<end;) {
        assert(end-position>=8);
        uint32_t size=be32(data,position);
        // These are small files produced by this test, not an import parser.
        assert(size>=8 && size<=end-position);
        result.push_back({position,position+size,std::string(data.begin()+position+4,data.begin()+position+8)});
        position+=size;
    }
    return result;
}
static Box child(const std::vector<uint8_t> &data, Box parent, const char *type)
{
    for (const auto &box : boxes(data,parent.start+8,parent.end))
        if (box.type==type) return box;
    assert(false); return {};
}
struct Timing {
    unsigned presentation=0, media=0, scale=0, stts_duration=0;
    std::vector<unsigned> deltas, edits;
};
static std::vector<Timing> read_timing(const std::string &path)
{
    std::ifstream file(path,std::ios::binary);
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    assert(file.eof() || file.good());
    Box moov{};
    for (const auto &box : boxes(data,0,data.size())) if (box.type=="moov") moov=box;
    assert(moov.end);
    std::vector<Timing> result;
    for (const auto &trak : boxes(data,moov.start+8,moov.end)) {
        if (trak.type!="trak") continue;
        Timing info;
        Box tkhd=child(data,trak,"tkhd"), mdia=child(data,trak,"mdia");
        Box mdhd=child(data,mdia,"mdhd");
        assert(data[tkhd.start+8]==0 && data[mdhd.start+8]==0);
        info.presentation=be32(data,tkhd.start+28);
        info.scale=be32(data,mdhd.start+20); info.media=be32(data,mdhd.start+24);
        Box minf=child(data,mdia,"minf"), stbl=child(data,minf,"stbl"), stts=child(data,stbl,"stts");
        unsigned count=be32(data,stts.start+12);
        assert(stts.end-stts.start==16+count*8);
        for (unsigned i=0;i<count;++i) {
            unsigned repeat=be32(data,stts.start+16+i*8), delta=be32(data,stts.start+20+i*8);
            assert(repeat && delta);
            info.stts_duration+=repeat*delta;
            for (unsigned j=0;j<repeat;++j) info.deltas.push_back(delta);
        }
        Box edts=child(data,trak,"edts"), elst=child(data,edts,"elst");
        assert(data[elst.start+8]==0);
        count=be32(data,elst.start+12);
        assert(elst.end-elst.start==16+count*12);
        for (unsigned i=0;i<count;++i) info.edits.push_back(be32(data,elst.start+16+i*12));
        result.push_back(info);
    }
    return result;
}
static KD_HANDLE add_audio(KD_HANDLE handle)
{
    KD_HANDLE track;
    k_mp4_track_info_s info{}; info.track_type=K_MP4_STREAM_AUDIO; info.time_scale=8000;
    info.audio_info={1,8000,16,2,K_MP4_CODEC_ID_G711A};
    assert(!kd_mp4_create_track(handle,&track,&info)); return track;
}
static void timing_tests(const std::string &path)
{
    KD_HANDLE video;
    KD_HANDLE handle=open_video(path,video);
    assert(lawrec_mp4_muxer_end_track(handle,video,33334)==-EINVAL); // No written sample.
    auto frame=sample(); assert(!kd_mp4_write_frame(handle,video,&frame));
    assert(lawrec_mp4_muxer_end_track(nullptr,video,33334)==-EINVAL);
    assert(lawrec_mp4_muxer_end_track(handle,nullptr,33334)==-EINVAL);
    assert(lawrec_mp4_muxer_end_track(handle,video,0)==-EINVAL);
    assert(lawrec_mp4_muxer_end_track(handle,video,UINT64_MAX)==-EINVAL);
    assert(!lawrec_mp4_muxer_end_track(handle,video,33334));
    close_video(handle,0);
    auto timing=read_timing(path);
    assert(timing.size()==1 && timing[0].scale==1000 && timing[0].media==34);
    assert(timing[0].presentation==34 && timing[0].stts_duration==34 && timing[0].deltas==std::vector<unsigned>{34});

    handle=open_video(path,video); frame=sample(); assert(!kd_mp4_write_frame(handle,video,&frame));
    KD_HANDLE audio=add_audio(handle);
    uint8_t sound[560]; for (unsigned i=0;i<sizeof(sound);++i) sound[i]=i%251;
    const unsigned lengths[]={160,320,80}, starts[]={0,20000,60000};
    unsigned offset=0;
    for (unsigned i=0;i<3;++i) {
        k_mp4_frame_data_s sample{}; sample.codec_id=K_MP4_CODEC_ID_G711A;
        sample.data=sound+offset; sample.data_length=lengths[i]; sample.time_stamp=starts[i];
        assert(!kd_mp4_write_frame(handle,audio,&sample)); offset+=lengths[i];
    }
    assert(!lawrec_mp4_muxer_end_track(handle,video,75000));
    assert(!lawrec_mp4_muxer_end_track(handle,audio,70000));
    lawrec_mp4_muxer_stats_t stats{};
    assert(!lawrec_mp4_muxer_stats(handle,&stats));
    assert(stats.track_count==2 && stats.sample_count==4 && stats.sample_capacity==2048);
    assert(stats.media_bytes>sizeof(sound) && stats.sample_index_bytes>=stats.sample_capacity);
    close_video(handle,0); timing=read_timing(path);
    assert(timing.size()==2 && timing[0].presentation==75 && timing[0].media==75);
    assert(timing[1].scale==1000 && timing[1].media==70 && timing[1].presentation==70);
    assert(timing[1].stts_duration==70 && timing[1].deltas==std::vector<unsigned>({20,40,10}));
    assert(timing[1].edits==std::vector<unsigned>{70});
    k_mp4_config_s config{}; config.config_type=K_MP4_CONFIG_DEMUXER;
    strcpy(config.demuxer_config.file_name,path.c_str());
    assert(!kd_mp4_create(&handle,&config));
    k_mp4_file_info_s file{}; assert(!kd_mp4_get_file_info(handle,&file) && file.duration==75);
    unsigned received=0; offset=0;
    for (;;) {
        k_mp4_frame_data_s sample{}; assert(!kd_mp4_get_frame(handle,&sample));
        if (sample.eof) break;
        if (sample.codec_id!=K_MP4_CODEC_ID_G711A) continue;
        assert(received<3 && sample.time_stamp==starts[received]/1000 && sample.data_length==lengths[received]);
        assert(!memcmp(sample.data,sound+offset,sample.data_length)); offset+=sample.data_length; ++received;
    }
    assert(received==3 && offset==sizeof(sound)); assert(!kd_mp4_destroy(handle));

    // A one-byte clipped tail needs a nonzero duration and a correct empty edit.
    handle=open_video(path,video); frame=sample(); assert(!kd_mp4_write_frame(handle,video,&frame));
    audio=add_audio(handle);
    k_mp4_frame_data_s tiny{}; tiny.codec_id=K_MP4_CODEC_ID_G711A;
    tiny.data=sound; tiny.data_length=1; tiny.time_stamp=50000;
    assert(!kd_mp4_write_frame(handle,audio,&tiny));
    assert(!lawrec_mp4_muxer_end_track(handle,video,60000));
    assert(!lawrec_mp4_muxer_end_track(handle,audio,50125));
    close_video(handle,0); timing=read_timing(path);
    assert(timing[1].media==1 && timing[1].presentation==51 && timing[1].stts_duration==1);
    assert(timing[1].edits==std::vector<unsigned>({50,1}));
    assert(!kd_mp4_create(&handle,&config)); assert(!kd_mp4_get_file_info(handle,&file) && file.duration==60);
    received=0;
    for (;;) {
        k_mp4_frame_data_s sample{}; assert(!kd_mp4_get_frame(handle,&sample));
        if (sample.eof) break;
        if (sample.codec_id==K_MP4_CODEC_ID_G711A) {
            assert(sample.data_length==1 && sample.data[0]==sound[0] && sample.time_stamp==50); ++received;
        }
    }
    assert(received==1); assert(!kd_mp4_destroy(handle));
    puts("PASS real SDK timing: explicit video end, clipped G711 lengths, STTS/mdhd/tkhd/edit durations and byte/PTS readback.");
}

int main(int argc, char **argv)
{
    assert(argc==2);
    alarm(10); // Fail a regression instead of leaving an SDK close loop running.
    std::string path=argv[1];
    KD_HANDLE track;
    KD_HANDLE handle=open_video(path,track);
    lawrec_mp4_muxer_stats_t stats{};
    assert(lawrec_mp4_muxer_stats(nullptr, &stats) == -EINVAL);
    assert(lawrec_mp4_muxer_stats(handle, nullptr) == -EINVAL);
    assert(!lawrec_mp4_muxer_stats(handle, &stats));
    // The SDK creates its writer video track lazily on the first SPS/PPS frame.
    assert(!stats.track_count && !stats.sample_count && !stats.sample_index_bytes);
    auto frame=sample();
    assert(!kd_mp4_write_frame(handle,track,&frame));
    assert(!lawrec_mp4_muxer_flush(handle));
    assert(!lawrec_mp4_muxer_stats(handle, &stats));
    assert(stats.track_count == 1 && stats.sample_count == 1 && stats.sample_capacity == 1024);
    assert(stats.media_bytes && stats.sample_index_bytes >= stats.sample_capacity);
    const auto first_index_bytes = stats.sample_index_bytes;
    frame.time_stamp=33000;
    assert(!kd_mp4_write_frame(handle,track,&frame));
    assert(!lawrec_mp4_muxer_stats(handle, &stats));
    assert(stats.sample_count == 2 && stats.sample_index_bytes == first_index_bytes);
    close_video(handle,0);
    struct stat st{}; assert(!stat(path.c_str(),&st) && st.st_size>0);
    // Validate produced container metadata with the actual SDK reader.
    k_mp4_config_s config{}; config.config_type=K_MP4_CONFIG_DEMUXER;
    strcpy(config.demuxer_config.file_name,path.c_str());
    assert(!kd_mp4_create(&handle,&config));
    k_mp4_file_info_s file_info{}; assert(!kd_mp4_get_file_info(handle,&file_info));
    assert(file_info.track_num==1);
    for (unsigned i=0;i<2;++i) {
        k_mp4_frame_data_s parsed{};
        assert(!kd_mp4_get_frame(handle,&parsed) && !parsed.eof);
        assert(parsed.codec_id==K_MP4_CODEC_ID_H264 && parsed.data_length>0 && parsed.time_stamp==i*33);
    }
    k_mp4_frame_data_s eof{}; assert(!kd_mp4_get_frame(handle,&eof) && eof.eof);
    assert(lawrec_mp4_muxer_flush(handle)==-EINVAL);
    assert(lawrec_mp4_muxer_stats(handle, &stats)==-EINVAL && !stats.sample_count);
    assert(!kd_mp4_destroy(handle));

    // Verify allocated index growth, not just used sample count/mdat bytes.
    handle=open_video(path,track);
    for (unsigned i=0; i<1025; ++i) {
        frame=sample(); frame.time_stamp=uint64_t(i)*33000;
        assert(!kd_mp4_write_frame(handle,track,&frame));
    }
    assert(!lawrec_mp4_muxer_stats(handle, &stats));
    assert(stats.sample_count==1025 && stats.sample_capacity==2048);
    assert(stats.sample_index_bytes==first_index_bytes*2 && stats.media_bytes>1025);
    close_video(handle,0);
    handle=open_video(path,track);
    assert(!lawrec_mp4_muxer_stats(handle, &stats) && !stats.sample_index_bytes);
    frame=sample(); assert(!kd_mp4_write_frame(handle,track,&frame));
    close_video(handle,0);

    // Exercise multi-block fast-start relocation, not only a tiny metadata box.
    handle=open_video(path,track);
    std::vector<unsigned char> padded(nal,nal+sizeof(nal)); padded.resize(16384,0x11);
    for (unsigned i=0;i<80;++i) {
        frame=sample(); frame.data=padded.data(); frame.data_length=padded.size(); frame.time_stamp=i*33000;
        assert(!kd_mp4_write_frame(handle,track,&frame));
    }
    close_video(handle,0);
    assert(!kd_mp4_create(&handle,&config));
    for (unsigned i=0;i<80;++i) {
        k_mp4_frame_data_s parsed{}; assert(!kd_mp4_get_frame(handle,&parsed) && !parsed.eof);
        assert(parsed.time_stamp==i*33 && parsed.data_length==padded.size());
        assert(!memcmp(parsed.data,padded.data(),padded.size()));
    }
    assert(!kd_mp4_get_frame(handle,&eof) && eof.eof);
    assert(!kd_mp4_destroy(handle));

    for (Fault injected : {WRITE,SHORT_WRITE,TELL}) {
        handle=open_video(path,track); frame=sample(); failures=0; fault=injected;
        int expected=injected==WRITE ? -ENOSPC : -EIO;
        int actual=kd_mp4_write_frame(handle,track,&frame);
        if (actual!=expected || !failures) fprintf(stderr,"write fault=%d expected=%d actual=%d failures=%u\n",fault,expected,actual,failures);
        assert(actual==expected && failures);
        fault=NONE;
        assert(lawrec_mp4_muxer_flush(handle)==expected); // Sticky per-handle fault.
        close_video(handle,expected);
    }
    handle=open_video(path,track); frame=sample(); assert(!kd_mp4_write_frame(handle,track,&frame));
    fault=FLUSH; assert(lawrec_mp4_muxer_flush(handle)==-ENOSPC);
    fault=NONE; close_video(handle,-ENOSPC);
    timing_tests(path);

    for (Fault injected : {WRITE,SEEK,CLOSE,TELL,READ}) {
        handle=open_video(path,track); frame=sample(); assert(!kd_mp4_write_frame(handle,track,&frame));
        assert(!lawrec_mp4_muxer_flush(handle));
        failures=0; fault=injected;
        close_video(handle,injected==WRITE ? -ENOSPC : -EIO);
        assert(failures);
    }
    // Two live muxers must not share sticky errors.
    handle=open_video(path,track); KD_HANDLE second_track;
    KD_HANDLE second=open_video(path+"-ok",second_track); frame=sample();
    fault=WRITE; assert(kd_mp4_write_frame(handle,track,&frame)==-ENOSPC);
    fault=NONE; assert(!kd_mp4_write_frame(second,second_track,&frame));
    close_video(handle,-ENOSPC); close_video(second,0);

    // A >2-MiB conversion must fail before a negative converter count reaches write().
    handle=open_video(path,track); frame=sample();
    std::vector<unsigned char> large(nal,nal+sizeof(nal)); large.resize(3*1024*1024,0x11);
    frame.data=large.data(); frame.data_length=large.size();
    assert(kd_mp4_write_frame(handle,track,&frame)==-EOVERFLOW);
    close_video(handle,0);
    handle=open_video("/dev/full",track); frame=sample();
    int full=kd_mp4_write_frame(handle,track,&frame);
    assert(full==0 || full==-ENOSPC);
    assert(lawrec_mp4_muxer_flush(handle)==-ENOSPC);
    close_video(handle,-ENOSPC);

    // Production creates G711A only after video, avoiding the SDK's G711U shortcut.
    handle=open_video(path,track); frame=sample(); assert(!kd_mp4_write_frame(handle,track,&frame));
    KD_HANDLE audio_track;
    k_mp4_track_info_s audio_info{}; audio_info.track_type=K_MP4_STREAM_AUDIO; audio_info.time_scale=8000;
    audio_info.audio_info={1,8000,16,2,K_MP4_CODEC_ID_G711A};
    assert(!kd_mp4_create_track(handle,&audio_track,&audio_info));
    unsigned char sound[320]; memset(sound,0xd5,sizeof(sound));
    k_mp4_frame_data_s audio_frame{}; audio_frame.codec_id=K_MP4_CODEC_ID_G711A;
    audio_frame.data=sound; audio_frame.data_length=sizeof(sound); audio_frame.time_stamp=40000;
    assert(!kd_mp4_write_frame(handle,audio_track,&audio_frame)); close_video(handle,0);
    config={}; config.config_type=K_MP4_CONFIG_DEMUXER; strcpy(config.demuxer_config.file_name,path.c_str());
    assert(!kd_mp4_create(&handle,&config)); assert(!kd_mp4_get_file_info(handle,&file_info) && file_info.track_num==2);
    unsigned audio_samples=0;
    for (;;) {
        k_mp4_frame_data_s parsed{}; assert(!kd_mp4_get_frame(handle,&parsed));
        if (parsed.eof) break;
        if (parsed.codec_id==K_MP4_CODEC_ID_G711A) {
            assert(parsed.time_stamp==40 && parsed.data_length==sizeof(sound));
            assert(!memcmp(parsed.data,sound,sizeof(sound))); ++audio_samples;
        }
    }
    assert(audio_samples==1); assert(!kd_mp4_destroy(handle));
    handle=open_video(path,track); frame=sample(); assert(!kd_mp4_write_frame(handle,track,&frame));
    assert(!kd_mp4_create_track(handle,&audio_track,&audio_info));
    fault=WRITE; assert(kd_mp4_write_frame(handle,audio_track,&audio_frame)==-ENOSPC);
    fault=NONE; close_video(handle,-ENOSPC);
    for (unsigned i=0;i<10;++i) {
        config={}; config.config_type=K_MP4_CONFIG_MUXER;
        strcpy(config.muxer_config.file_name,"/nonexistent-lawrec-dir/file.part");
        handle=reinterpret_cast<KD_HANDLE>(1);
        observe_instance=true;
        assert(kd_mp4_create(&handle,&config)==-ENOENT && !handle);
        observe_instance=false; assert(!instance_allocation);
    }
    config={}; config.config_type=K_MP4_CONFIG_MUXER;
    strcpy(config.muxer_config.file_name,path.c_str());
    for (unsigned allocation : {1,2}) {
        observe_instance=true; fail_writer_allocation=allocation;
        assert(kd_mp4_create(&handle,&config)==-ENOMEM && !handle);
        observe_instance=false; assert(!fail_writer_allocation && !instance_allocation);
    }
    observe_instance=true; fault=WRITE;
    assert(kd_mp4_create(&handle,&config)==-ENOSPC && !handle);
    observe_instance=false; fault=NONE; assert(!instance_allocation);
    unlink(path.c_str()); unlink((path+"-ok").c_str());
    puts("PASS real SDK muxer: H264/G711A container and samples, write/short-write/read/seek/tell/flush/finalize/close errors, isolation, bounds, allocation cleanup.");
}
