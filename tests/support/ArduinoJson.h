#pragma once

class JsonVariantProxy {
public:
    template <typename T>
    JsonVariantProxy& operator=(T&&) {
        return *this;
    }
};

class JsonObject {
public:
    JsonVariantProxy operator[](const char*) {
        return JsonVariantProxy{};
    }
};

