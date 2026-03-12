#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

class String {
public:
    String() = default;
    String(const String&) = default;
    String(String&&) noexcept = default;
    String(const char* value) : value_(value != nullptr ? value : "") {}
    String(const std::string& value) : value_(value) {}
    String(char value) : value_(1, value) {}
    String(unsigned char value) : value_(1, static_cast<char>(value)) {}

    template <typename T,
              typename std::enable_if<std::is_arithmetic<T>::value &&
                                          !std::is_same<typename std::decay<T>::type, char>::value &&
                                          !std::is_same<typename std::decay<T>::type, unsigned char>::value,
                                      int>::type = 0>
    String(T value) : value_(std::to_string(value)) {}

    String& operator=(const String&) = default;
    String& operator=(String&&) noexcept = default;

    size_t length() const {
        return value_.size();
    }

    bool isEmpty() const {
        return value_.empty();
    }

    void reserve(size_t size) {
        value_.reserve(size);
    }

    void toUpperCase() {
        std::transform(value_.begin(), value_.end(), value_.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
    }

    int indexOf(const char* needle) const {
        if (needle == nullptr) {
            return -1;
        }
        const size_t pos = value_.find(needle);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    bool endsWith(const char* suffix) const {
        if (suffix == nullptr) {
            return false;
        }
        const size_t suffixLength = std::strlen(suffix);
        return value_.size() >= suffixLength &&
               value_.compare(value_.size() - suffixLength, suffixLength, suffix) == 0;
    }

    String substring(size_t start) const {
        return substring(start, value_.size());
    }

    String substring(size_t start, size_t end) const {
        if (start >= value_.size() || start >= end) {
            return String("");
        }
        const size_t clampedEnd = std::min(end, value_.size());
        return String(value_.substr(start, clampedEnd - start));
    }

    const char* c_str() const {
        return value_.c_str();
    }

    char& operator[](size_t index) {
        return value_[index];
    }

    char operator[](size_t index) const {
        return value_[index];
    }

    String& operator+=(const String& other) {
        value_ += other.value_;
        return *this;
    }

    String& operator+=(const char* other) {
        if (other != nullptr) {
            value_ += other;
        }
        return *this;
    }

    String& operator+=(char other) {
        value_ += other;
        return *this;
    }

    template <typename T,
              typename std::enable_if<std::is_arithmetic<T>::value &&
                                          !std::is_same<typename std::decay<T>::type, char>::value &&
                                          !std::is_same<typename std::decay<T>::type, unsigned char>::value,
                                      int>::type = 0>
    String& operator+=(T value) {
        value_ += std::to_string(value);
        return *this;
    }

    friend bool operator==(const String& lhs, const String& rhs) {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator==(const String& lhs, const char* rhs) {
        return lhs.value_ == (rhs != nullptr ? rhs : "");
    }

    friend bool operator==(const char* lhs, const String& rhs) {
        return rhs == lhs;
    }

    friend bool operator!=(const String& lhs, const String& rhs) {
        return !(lhs == rhs);
    }

    friend bool operator!=(const String& lhs, const char* rhs) {
        return !(lhs == rhs);
    }

    friend bool operator!=(const char* lhs, const String& rhs) {
        return !(lhs == rhs);
    }

    friend String operator+(String lhs, const String& rhs) {
        lhs += rhs;
        return lhs;
    }

private:
    std::string value_;
};

inline bool isDigit(int c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

