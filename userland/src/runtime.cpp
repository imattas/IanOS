extern "C" void* memcpy(void* destination, const void* source, unsigned long long count) {
    auto* dst = static_cast<unsigned char*>(destination);
    const auto* src = static_cast<const unsigned char*>(source);
    for (unsigned long long i = 0; i < count; ++i) dst[i] = src[i];
    return destination;
}

extern "C" void* memmove(void* destination, const void* source, unsigned long long count) {
    auto* dst = static_cast<unsigned char*>(destination);
    const auto* src = static_cast<const unsigned char*>(source);
    if (dst < src) {
        for (unsigned long long i = 0; i < count; ++i) dst[i] = src[i];
    } else if (dst > src) {
        for (unsigned long long i = count; i != 0; --i) dst[i - 1] = src[i - 1];
    }
    return destination;
}

extern "C" void* memset(void* destination, int value, unsigned long long count) {
    auto* dst = static_cast<unsigned char*>(destination);
    for (unsigned long long i = 0; i < count; ++i) dst[i] = static_cast<unsigned char>(value);
    return destination;
}
