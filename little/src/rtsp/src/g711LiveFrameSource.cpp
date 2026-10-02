#include "g711LiveFrameSource.h"

G711LiveFrameSource* G711LiveFrameSource::createNew(UsageEnvironment &env, size_t queue_size) {
    auto *source = new G711LiveFrameSource(env, queue_size);
    if (source->deliveryError()) { Medium::close(source); return nullptr; }
    return source;
}

G711LiveFrameSource::G711LiveFrameSource(UsageEnvironment &env, size_t queue_size) : LiveFrameSource(env, queue_size) 
{}
