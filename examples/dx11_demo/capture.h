// Back buffer -> PNG capture with zero dependencies (stored/uncompressed deflate).
// Used by the demo's --capture mode so tools and AI agents can verify UI changes
// without a visible window.
#pragma once
#include <d3d11.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace capture {

inline uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

inline void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    PutBE32(out, uint32_t(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    PutBE32(out, Crc32(out.data() + start, out.size() - start));
}

// rgb: width*height*3 bytes, top-down.
inline bool WritePng(const std::wstring& path, const uint8_t* rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
    }
    std::vector<uint8_t> z{ 0x78, 0x01 };
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + n));
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    PutBE32(z, (b << 16) | a);

    std::vector<uint8_t> png{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    std::vector<uint8_t> ihdr;
    PutBE32(ihdr, w); PutBE32(ihdr, h);
    ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

// Copies an R8G8B8A8 / B8G8R8A8 texture to the CPU and writes it as PNG.
inline bool SaveTexturePng(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return false;
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) { staging->Release(); return false; }
    std::vector<uint8_t> rgb(size_t(d.Width) * d.Height * 3);
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (UINT x = 0; x < d.Width; ++x) {
            uint8_t* o = &rgb[(size_t(y) * d.Width + x) * 3];
            o[0] = row[x * 4 + (bgra ? 2 : 0)];
            o[1] = row[x * 4 + 1];
            o[2] = row[x * 4 + (bgra ? 0 : 2)];
        }
    }
    ctx->Unmap(staging, 0);
    staging->Release();
    return WritePng(path, rgb.data(), d.Width, d.Height);
}

} // namespace capture
