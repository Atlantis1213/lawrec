# Build a narrow, project-owned adaptation of the selected SDK implementation.
# Never patch the SDK tree or silently apply these replacements to a new layout.
function(lawrec_prepare_mp4 input output)
    get_filename_component(sdk_src "${input}" DIRECTORY)
    set(writer "${sdk_src}/libmov/source/mov-writer.c")
    set(stts "${sdk_src}/libmov/source/mov-stts.c")
    set(elst "${sdk_src}/libmov/source/mov-elst.c")
    file(SHA256 "${input}" wrapper_hash)
    file(SHA256 "${writer}" writer_hash)
    file(SHA256 "${stts}" stts_hash)
    file(SHA256 "${elst}" elst_hash)
    if(NOT wrapper_hash STREQUAL "af5d06f10fc49114fb0e8b878fd5c01f593fec29f1ff2a286afeefbcd3bdff2f" OR
       NOT writer_hash STREQUAL "0147de762906ce1b844ab4fa10dba0104f8db2b45d6cef482757f254a248351d" OR
       NOT stts_hash STREQUAL "cbe3055565e712659d668d0d20eacda51ab03f1a20941a19930c13e264e2319c" OR
       NOT elst_hash STREQUAL "3328b7863eee269e7bea5313455db5041ac745181e71fd0d8203e1e08e23f2f8")
        message(FATAL_ERROR "MP4 SDK source fingerprint changed; review the adaptation and real-SDK tests before upgrading")
    endif()
    file(READ "${input}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    function(replace_once before after)
        string(FIND "${source}" "${before}" position)
        if(position LESS 0)
            message(FATAL_ERROR "MP4 SDK adaptation no longer matches: ${before}")
        endif()
        string(LENGTH "${before}" length)
        math(EXPR rest "${position} + ${length}")
        string(SUBSTRING "${source}" ${rest} -1 tail)
        string(FIND "${tail}" "${before}" repeated)
        if(NOT repeated LESS 0)
            message(FATAL_ERROR "Ambiguous MP4 SDK adaptation: ${before}")
        endif()
        string(REPLACE "${before}" "${after}" source "${source}")
        set(source "${source}" PARENT_SCOPE)
    endfunction()

    replace_once("#include <assert.h>" "#include <assert.h>\n#include <errno.h>\n#include <stdint.h>\nstruct mov_writer_t;\nint lawrec_mov_writer_destroy(struct mov_writer_t *writer);\nint lawrec_mov_writer_end_track(struct mov_writer_t *writer, int track, uint64_t end_us);")
    replace_once("#include <stdint.h>" "#include <stdint.h>\n#include \"lawrec_mp4_io.h\"\nstruct mov_writer_t;\nint lawrec_mov_writer_stats(const struct mov_writer_t *writer, lawrec_mp4_muxer_stats_t *stats);")
    replace_once("    FILE *fp;\n} k_muxer_instance;" "    FILE *fp;\n    int io_error;\n} k_muxer_instance;")
    string(FIND "${source}" "static int mov_file_read(" start)
    string(FIND "${source}" "static int mov_file_cache_read(" end)
    if(start LESS 0 OR end LESS start)
        message(FATAL_ERROR "MP4 SDK stdio callbacks no longer match")
    endif()
    math(EXPR count "${end} - ${start}")
    string(SUBSTRING "${source}" ${start} ${count} callbacks)
    replace_once("${callbacks}" [=[static int muxer_error(k_muxer_instance *muxer)
{
    if (!muxer->io_error) muxer->io_error = -(errno ? errno : EIO);
    return muxer->io_error;
}

static int mov_file_read(void *context, void *data, uint64_t bytes)
{
    k_muxer_instance *muxer = context;
    if (muxer->io_error) return muxer->io_error;
    errno = 0;
    if (bytes == fread(data, 1, bytes, muxer->fp)) return 0;
    return ferror(muxer->fp) || errno ? muxer_error(muxer) : -ENODATA;
}

static int mov_file_write(void *context, const void *data, uint64_t bytes)
{
    k_muxer_instance *muxer = context;
    if (muxer->io_error) return muxer->io_error;
    errno = 0;
    if (bytes == fwrite(data, 1, bytes, muxer->fp)) return 0;
    return muxer_error(muxer);
}

static int mov_file_seek(void *context, int64_t offset)
{
    k_muxer_instance *muxer = context;
    if (muxer->io_error) return muxer->io_error;
    errno = 0;
    if (!fseek(muxer->fp, offset, offset >= 0 ? SEEK_SET : SEEK_END)) return 0;
    return muxer_error(muxer);
}

static int64_t mov_file_tell(void *context)
{
    k_muxer_instance *muxer = context;
    if (muxer->io_error) return muxer->io_error;
    errno = 0;
    long position = ftell(muxer->fp);
    return position < 0 ? muxer_error(muxer) : position;
}

/* A stdio buffer can defer ENOSPC until flush; no fsync/durability claim here. */
int lawrec_mp4_muxer_flush(KD_HANDLE handle)
{
    k_mp4_instance *instance = handle;
    if (!instance || instance->instance_type != K_MP4_CONFIG_MUXER ||
        !instance->muxer_instance.fp) return -EINVAL;
    k_muxer_instance *muxer = &instance->muxer_instance;
    if (muxer->io_error) return muxer->io_error;
    errno = 0;
    if (fflush(muxer->fp) || ferror(muxer->fp)) return muxer_error(muxer);
    return 0;
}

int lawrec_mp4_muxer_end_track(KD_HANDLE handle, KD_HANDLE track_handle, uint64_t end_us)
{
    k_mp4_instance *instance = handle;
    if (!instance || !track_handle || instance->instance_type != K_MP4_CONFIG_MUXER)
        return -EINVAL;
    k_muxer_instance *muxer = &instance->muxer_instance;
    if (muxer->io_error) return muxer->io_error;
    if (!muxer->mov || !muxer->mov->mov) return -EINVAL;
    for (int i = 0; i < MAX_TRACK_NUM; ++i) {
        if (muxer->track[i] == track_handle)
            return lawrec_mov_writer_end_track(muxer->mov->mov, muxer->track[i]->add_to_mp4, end_us);
    }
    return -EINVAL;
}

int lawrec_mp4_muxer_stats(KD_HANDLE handle, lawrec_mp4_muxer_stats_t *stats)
{
    if (!stats) return -EINVAL;
    memset(stats, 0, sizeof(*stats));
    k_mp4_instance *instance = handle;
    if (!instance || instance->instance_type != K_MP4_CONFIG_MUXER ||
        !instance->muxer_instance.mov || !instance->muxer_instance.mov->mov)
        return -EINVAL;
    return lawrec_mov_writer_stats(instance->muxer_instance.mov->mov, stats);
}

]=])

    replace_once("    if (!mp4_cfg) {" "    if (!mp4_handle || !mp4_cfg) {")
    replace_once("    k_mp4_instance *mp4_instance = NULL;" "    *mp4_handle = NULL;\n    k_mp4_instance *mp4_instance = NULL;")
    replace_once([=[        printf("kd_mp4_create: create mp4 instance failed.\n");
        return -1;]=] [=[        printf("kd_mp4_create: create mp4 instance failed.\n");
        return -ENOMEM;]=])
    replace_once([=[                printf("kd_mp4_create: output file %s open failed.\n", mp4_cfg->muxer_config.file_name);
                return -1;]=] [=[                int error = -(errno ? errno : EIO);
                printf("kd_mp4_create: output file %s open failed.\n", mp4_cfg->muxer_config.file_name);
                free(mp4_instance);
                return error;]=])
    replace_once("            mp4_instance->muxer_instance.mov = mp4_writer_create" "            mp4_instance->muxer_instance.fp = fp;\n            mp4_instance->muxer_instance.mov = mp4_writer_create")
    replace_once("mov_file_buffer(), fp, MOV_FLAG_FASTSTART" "mov_file_buffer(), &mp4_instance->muxer_instance, MOV_FLAG_FASTSTART")
    replace_once([=[                printf("kd_mp4_create: create mp4 writer failed.\n");
                return -1;]=] [=[                int error = mp4_instance->muxer_instance.io_error;
                free(mp4_instance->muxer_instance.mov);
                fclose(fp);
                free(mp4_instance);
                return error ? error : -ENOMEM;]=])
    replace_once("            if (!mp4_instance->muxer_instance.mov) {" [=[            if (!mp4_instance->muxer_instance.mov ||
                (!mp4_instance->muxer_instance.mov->mov && !mp4_instance->muxer_instance.mov->fmp4)) {]=])
    replace_once("            mp4_instance->muxer_instance.fp = fp;\n            break;" [=[            if (mp4_instance->muxer_instance.io_error) {
                int error = mp4_instance->muxer_instance.io_error;
                kd_mp4_destroy(mp4_instance);
                return error;
            }
            break;]=])

    replace_once("int kd_mp4_destroy(KD_HANDLE mp4_handle) {" "int kd_mp4_destroy(KD_HANDLE mp4_handle) {\n    int result = 0;")
    replace_once("                mp4_writer_destroy(mp4_instance->muxer_instance.mov);" [=[                struct mp4_writer_t *writer = mp4_instance->muxer_instance.mov;
                if (writer->mov) {
                    result = lawrec_mov_writer_destroy(writer->mov);
                    free(writer);
                } else mp4_writer_destroy(writer);]=])
    replace_once([=[                fclose(mp4_instance->muxer_instance.fp);
                mp4_instance->muxer_instance.fp = NULL;]=] [=[                int flush_error = lawrec_mp4_muxer_flush(mp4_instance);
                if (flush_error) result = flush_error;
                errno = 0;
                if (fclose(mp4_instance->muxer_instance.fp) && !result)
                    result = -(errno ? errno : EIO);
                mp4_instance->muxer_instance.fp = NULL;]=])
    replace_once("    return 0;\n}\n\nint kd_mp4_create_track" "    return result;\n}\n\nint kd_mp4_create_track")
    replace_once([=[        printf("kd_mp4_create_track: mp4 already cannot creat new track.\n");
        return -1;]=] [=[        printf("kd_mp4_create_track: mp4 already cannot creat new track.\n");
        free(track);
        *track_handle = NULL;
        return -ENOSPC;]=])

    replace_once("int kd_mp4_write_frame(KD_HANDLE mp4_handle, KD_HANDLE track_handle, k_mp4_frame_data_s *frame_data) {" [=[int kd_mp4_write_frame(KD_HANDLE mp4_handle, KD_HANDLE track_handle, k_mp4_frame_data_s *frame_data) {
    if (!frame_data || !frame_data->data || !frame_data->data_length) return -EINVAL;
    if (frame_data->data_length > 2 * 1024 * 1024) return -EOVERFLOW;]=])
    replace_once([=[static k_track_ctx * get_audio_track(KD_HANDLE mp4_handle) {
    if (mp4_handle == NULL) {
        return -1;
    }]=] [=[static k_track_ctx * get_audio_track(KD_HANDLE mp4_handle) {
    if (mp4_handle == NULL) {
        return NULL;
    }]=])
    replace_once([=[    if (mp4_instance->instance_type == K_MP4_CONFIG_DEMUXER) {
        return -1;
    }]=] [=[    if (mp4_instance->instance_type == K_MP4_CONFIG_DEMUXER) {
        return NULL;
    }]=])
    foreach(codec IN ITEMS h264 hevc)
        if(codec STREQUAL "h264")
            set(converter "h264_annexbtomp4(&track->avc, ptr, ptr_len, s_buffer, s_buffer_size, &vcl, &update)")
        else()
            set(converter "h265_annexbtomp4(&track->hevc, ptr, ptr_len, s_buffer, s_buffer_size, &vcl, &update)")
        endif()
        replace_once("            int n = ${converter};" "            int n = ${converter};\n            if (n <= 0 || (size_t)n > s_buffer_size) return -EOVERFLOW;")
    endforeach()
    # The SDK discards four writer results (H264/H265/G711A/G711U).
    string(REGEX MATCHALL "            mp4_writer_write\\([^\n;]+\\)" writes "${source}")
    list(LENGTH writes write_count)
    if(NOT write_count EQUAL 4)
        message(FATAL_ERROR "Expected four SDK muxer writes, found ${write_count}")
    endif()
    string(REGEX REPLACE "            mp4_writer_write\\(([^\n]+)\\);" "            int status = mp4_writer_write(\\1);\n            if (mp4_instance->muxer_instance.io_error) return mp4_instance->muxer_instance.io_error;\n            if (status) return status < 0 ? status : -EIO;" source "${source}")

    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${output}" "/* Generated SDK muxer I/O adaptation; see record/prepare_mp4.cmake. */\n${source}")

    # The SDK fast-start close path can loop forever with a zero-byte move after
    # an I/O fault. Keep fast-start on healthy files; abort only failed finalization.
    file(READ "${writer}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    replace_once("#include <errno.h>" "#include <errno.h>\n#include \"lawrec_mp4_io.h\"")
    replace_once("void mov_writer_destroy(struct mov_writer_t* writer)" "int lawrec_mov_writer_destroy(struct mov_writer_t* writer)")
    replace_once("\tmov = &writer->mov;\n\n\t// finish mdat box" "\tmov = &writer->mov;\n\tif (mov_buffer_error(&mov->io)) goto cleanup;\n\n\t// finish mdat box")
    replace_once("\t// finish sample info" "\tif (mov_buffer_error(&mov->io)) goto cleanup;\n\t// finish sample info")
    replace_once([=[		// pts in ms
		track->mdhd.duration = (track->samples[track->sample_count - 1].dts - track->samples[0].dts);
		if (track->sample_count > 1)
		{
			// duration += 3/4 * avg-duration + 1/4 * last-frame-duration
			track->mdhd.duration += track->mdhd.duration * 3 / (track->sample_count - 1) / 4 + (track->samples[track->sample_count - 1].dts - track->samples[track->sample_count - 2].dts) / 4;
		}
		//track->mdhd.duration = track->mdhd.duration * track->mdhd.timescale / 1000;
		track->tkhd.duration = track->mdhd.duration * mov->mvhd.timescale / track->mdhd.timescale;]=] [=[		// The extra allocated slot records an exclusive end, not another sample.
		int64_t end = track->samples[track->sample_count].dts;
		if (end <= track->samples[track->sample_count - 1].dts) {
			mov->io.error = -EINVAL;
			goto cleanup;
		}
		track->mdhd.duration = end - track->samples[0].dts;
		// tkhd/mvhd include the edit-list delay; mdhd/STTS contain media only.
		track->tkhd.duration = end * mov->mvhd.timescale / track->mdhd.timescale;]=])
    replace_once("\tsample = &mov->track->samples[mov->track->sample_count++];" [=[	if (pts < 0 || dts < 0 ||
		(mov->track->sample_count && dts <= mov->track->samples[mov->track->sample_count - 1].dts))
		return -EINVAL;
	sample = &mov->track->samples[mov->track->sample_count++];]=])
    replace_once("\twriter->mdat_size += bytes; // update media data size" [=[	int64_t duration = mov->track->sample_count > 1 ?
		dts - mov->track->samples[mov->track->sample_count - 2].dts : 1;
	const struct mov_sample_entry_t *entry = &mov->track->stsd.entries[0];
	if (entry->object_type_indication == MOV_OBJECT_G711a || entry->object_type_indication == MOV_OBJECT_G711u) {
		uint64_t rate = (uint64_t)(entry->u.audio.samplerate >> 16) * entry->u.audio.channelcount;
		if (!rate || bytes % entry->u.audio.channelcount) { mov->io.error = -EINVAL; return -EINVAL; }
		duration = ((uint64_t)bytes * mov->track->mdhd.timescale + rate - 1) / rate;
	}
	if (duration <= 0 || dts > INT64_MAX - duration) { mov->io.error = -EOVERFLOW; return -EOVERFLOW; }
	mov->track->samples[mov->track->sample_count].dts = dts + duration;
	writer->mdat_size += bytes; // update media data size]=])
    replace_once("\tmov_write_moov(mov);\n\toffset2 = mov_buffer_tell(&mov->io);" "\tmov_write_moov(mov);\n\tif (mov_buffer_error(&mov->io)) goto cleanup;\n\toffset2 = mov_buffer_tell(&mov->io);\n\tif (mov_buffer_error(&mov->io)) goto cleanup;")
    replace_once("\t\tassert(mov_buffer_tell(&mov->io) == offset2 + co64);" "\t\tif (mov_buffer_error(&mov->io)) goto cleanup;\n\t\tassert(mov_buffer_tell(&mov->io) == offset2 + co64);")
    replace_once("\tmov_write_tail(mov);\n\tfor (i = 0; i < mov->track_count; i++)" "\tmov_write_tail(mov);\ncleanup:;\n\tint result = mov_buffer_error(&mov->io);\n\tfor (i = 0; i < mov->track_count; i++)")
    replace_once("\tassert(bytes < INT32_MAX);" "\tif (!bytes) return from == to ? 0 : -EIO;\n\tif (bytes >= INT32_MAX || from < to) return -EOVERFLOW;")
    replace_once("    mov_buffer_read(&mov->io, buffer[1], bytes);\n\n\tj = 0;" "    mov_buffer_read(&mov->io, buffer[1], bytes);\n\tif (mov_buffer_error(&mov->io)) goto move_done;\n\n\tj = 0;")
    # The last read is unused and may go past EOF on a short healthy recording.
    replace_once("        mov_buffer_seek(&mov->io, i+bytes);\n        mov_buffer_read(&mov->io, buffer[j], bytes);\n        j ^= 1;" "        if (i + bytes < from) {\n            mov_buffer_seek(&mov->io, i+bytes);\n            mov_buffer_read(&mov->io, buffer[j], bytes);\n            if (mov_buffer_error(&mov->io)) goto move_done;\n        }\n        j ^= 1;")
    replace_once("\tfree(ptr);\n\treturn mov_buffer_error(&mov->io);" "move_done:\n\tfree(ptr);\n\treturn mov_buffer_error(&mov->io);")
    replace_once("\t\tmov_writer_move(mov, writer->mdat_offset, offset, (size_t)(offset2 - offset));" "\t\tint status = mov_writer_move(mov, writer->mdat_offset, offset, (size_t)(offset2 - offset));\n\t\tif (status) mov->io.error = status;")
    # Let the outer wrapper observe even non-stdio failures (e.g. move allocation).
    replace_once("\tfree(writer);\n}" "\tfree(writer);\n\treturn result;\n}\n\nvoid mov_writer_destroy(struct mov_writer_t *writer)\n{\n    (void)lawrec_mov_writer_destroy(writer);\n}")
    string(APPEND source [=[

int lawrec_mov_writer_end_track(struct mov_writer_t *writer, int index, uint64_t end_us)
{
    if (!writer || index < 0 || index >= (int)writer->mov.track_count) return -EINVAL;
    struct mov_track_t *track = &writer->mov.tracks[index];
    if (!track->sample_count || track->mdhd.timescale != 1000) return -EINVAL;
    uint64_t end = end_us/1000 + (end_us%1000 != 0);
    int64_t last = track->samples[track->sample_count - 1].dts;
    if (end > INT64_MAX || end <= (uint64_t)last || end - last > UINT32_MAX)
        return -EINVAL;
    track->samples[track->sample_count].dts = end;
    return mov_buffer_error(&writer->mov.io);
}

int lawrec_mov_writer_stats(const struct mov_writer_t *writer, lawrec_mp4_muxer_stats_t *stats)
{
    if (!writer || !stats || writer->mov.track_count < 0) return -EINVAL;
    memset(stats, 0, sizeof(*stats));
    stats->track_count = writer->mov.track_count;
    stats->media_bytes = writer->mdat_size;
    for (int i = 0; i < writer->mov.track_count; ++i) {
        const struct mov_track_t *track = &writer->mov.tracks[i];
        stats->sample_count += track->sample_count;
        stats->sample_capacity += track->sample_offset;
        stats->sample_index_bytes += (uint64_t)track->sample_offset * sizeof(struct mov_sample_t);
    }
    return 0;
}
]=])
    file(WRITE "${directory}/mov-writer.c" "/* Generated SDK checked finalization and track ends; see record/prepare_mp4.cmake. */\n${source}")

    file(READ "${stts}" source)
    replace_once("\t\tassert(track->samples[i + 1].dts >= track->samples[i].dts || i + 1 == track->sample_count);" "\t\tassert(track->samples[i + 1].dts > track->samples[i].dts);")
    replace_once("        delta = (uint32_t)(i + 1 < track->sample_count && track->samples[i + 1].dts > track->samples[i].dts ? track->samples[i + 1].dts - track->samples[i].dts : 1);" "        delta = (uint32_t)(track->samples[i + 1].dts - track->samples[i].dts);")
    file(WRITE "${directory}/mov-stts.c" "/* Generated last-sample duration adaptation; see record/prepare_mp4.cmake. */\n${source}")

    file(READ "${elst}" source)
    # tkhd is the whole presentation, including its empty leading edit.
    # The playable edit must contain only mdhd's media duration.
    replace_once("\t\tmov_buffer_w64(&mov->io, track->tkhd.duration);" "\t\tmov_buffer_w64(&mov->io, track->mdhd.duration * mov->mvhd.timescale / track->mdhd.timescale);")
    replace_once("\t\tmov_buffer_w32(&mov->io, (uint32_t)track->tkhd.duration);" "\t\tmov_buffer_w32(&mov->io, (uint32_t)(track->mdhd.duration * mov->mvhd.timescale / track->mdhd.timescale));")
    file(WRITE "${directory}/mov-elst.c" "/* Generated edit-list media duration; see record/prepare_mp4.cmake. */\n${source}")
endfunction()

if(DEFINED LAWREC_MP4_INPUT AND DEFINED LAWREC_MP4_OUTPUT)
    lawrec_prepare_mp4("${LAWREC_MP4_INPUT}" "${LAWREC_MP4_OUTPUT}")
endif()
