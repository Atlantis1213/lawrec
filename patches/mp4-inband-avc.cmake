# Only the private build copy is extended. SDK sources stay read-only.
file(READ "${MP4_OUT}/source/mov-writer.c" source)
string(APPEND source [=[

/* K230 repeats parameter sets with changing IDs; retain them in avc3 samples. */
int demo_mov_writer_inband_avc(struct mov_writer_t* writer, int index)
{
    struct mov_track_t* track;
    if (!writer || index < 0 || index >= writer->mov.track_count)
        return -EINVAL;
    track = &writer->mov.tracks[index];
    if (track->tag != MOV_H264 || track->sample_count)
        return -EINVAL;
    track->tag = MOV_TAG('a', 'v', 'c', '3');
    return 0;
}
]=])
file(WRITE "${MP4_OUT}/source/mov-writer.c" "${source}")
