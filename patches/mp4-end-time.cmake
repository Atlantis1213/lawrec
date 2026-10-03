# Only private, normalized libmov copies in the build directory are patched.
function(mp4_replace old new)
    string(FIND "${source}" "${old}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "SDK libmov changed; cannot apply end-time patch")
    endif()
    string(REPLACE "${old}" "${new}" source "${source}")
    set(source "${source}" PARENT_SCOPE)
endfunction()

file(READ "${MP4_OUT}/source/mov-internal.h" source)
mp4_replace("    int64_t start_dts; // write fmp4 only"
    "    int64_t start_dts; // write fmp4 only\n    int64_t demo_end_dts; // explicit final sample boundary, media time scale")
file(WRITE "${MP4_OUT}/source/mov-internal.h" "${source}")

file(READ "${MP4_OUT}/source/mov-writer.c" source)
mp4_replace("\t\t//track->mdhd.duration = track->mdhd.duration * track->mdhd.timescale / 1000;"
    "\t\tif (track->demo_end_dts > track->samples[track->sample_count - 1].dts)\n\t\t\ttrack->mdhd.duration = track->demo_end_dts - track->samples[0].dts;\n\t\t//track->mdhd.duration = track->mdhd.duration * track->mdhd.timescale / 1000;")
string(APPEND source [=[

/* Demo-only extension. All public input timestamps remain milliseconds. */
int demo_mov_writer_end_track(struct mov_writer_t* writer, int index, int64_t end_ms)
{
    struct mov_track_t* track;
    if (!writer || index < 0 || index >= writer->mov.track_count || end_ms < 0)
        return -EINVAL;
    track = &writer->mov.tracks[index];
    if (!track->sample_count || end_ms > INT64_MAX / track->mdhd.timescale)
        return -ERANGE;
    end_ms = end_ms * track->mdhd.timescale / 1000;
    if (end_ms <= track->samples[track->sample_count - 1].dts)
        return -ERANGE;
    track->demo_end_dts = end_ms;
    return 0;
}
]=])
file(WRITE "${MP4_OUT}/source/mov-writer.c" "${source}")

file(READ "${MP4_OUT}/source/mov-stts.c" source)
mp4_replace("assert(track->samples[i + 1].dts >= track->samples[i].dts || i + 1 == track->sample_count);"
    "assert(i + 1 == track->sample_count || track->samples[i + 1].dts >= track->samples[i].dts);")
mp4_replace("delta = (uint32_t)(i + 1 < track->sample_count && track->samples[i + 1].dts > track->samples[i].dts ? track->samples[i + 1].dts - track->samples[i].dts : 1);"
    "delta = (uint32_t)(i + 1 < track->sample_count && track->samples[i + 1].dts > track->samples[i].dts ? track->samples[i + 1].dts - track->samples[i].dts : (track->demo_end_dts > track->samples[i].dts ? track->demo_end_dts - track->samples[i].dts : 1));")
file(WRITE "${MP4_OUT}/source/mov-stts.c" "${source}")
