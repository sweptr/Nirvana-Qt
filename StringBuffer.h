#ifndef STRING_BUFFER_H_
#define STRING_BUFFER_H_

#include <cstddef>
#include <algorithm>

template <class Ch>
class StringBuffer {
public:
    StringBuffer(size_t size) : size_t(size) {
        p_ = new Ch[size + 1];
    }

    ~StringBuffer() {
        delete [] p_;
    }

    StringBuffer(StringBuffer &&other) : p_(other.p_), size_(other.size_) {
        other.p_ = nullptr;
        other.size_ = 0;
        return *this;
    }

    StringBuffer& operator=(StringBuffer &&rhs) {
        rhs.swap(*this);
        return *this;
    }

public:
    size_t size() const {
        return size_;
    }

    Ch *begin() { return p_; }
    Ch *end() { return p_ + size_; }
    const Ch *const begin() { return p_; }
    const Ch *const end() { return p_ + size_; }
    Ch *data() { return p_; }
    const Ch *data() const { return p_; }

public:
    void swap(StringBuffer &other) {
        using std::swap;
        swap(p_, other.p_);
        swap(size_, other.size_);
    }

private:
    Ch *p_;
    size_t size_;
};

#endif

