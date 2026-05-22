#pragma once
// docs/design/C2PA.md §3.2 — render a Stellar G-address (or any
// short payload) as a QR code SkImage for in-app display alongside
// the wallet's funding-address copy button. The artist can scan with
// any Stellar wallet (Lobstr, Solar, Freighter) to skip a typo-prone
// 56-char paste.
//
// Backed by deps/qrcodegen (Nayuki, MIT). Returns an immutable
// in-memory SkImage suitable for MemoryImageDisplay — refresh by
// passing a new sk_sp<SkImage>.

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <string_view>

namespace C2PA {

// Encode `payload` (UTF-8, must be non-empty + short enough for the
// QR encoder's Low ECC bucket — Stellar G-addresses are 56 chars,
// well within range) and render as an `out_px` × `out_px` RGBA8888
// SkImage. The image includes the QR spec's 4-module quiet-zone
// border. Returns nullptr on encoder failure.
sk_sp<SkImage> render_qr_image(std::string_view payload, int out_px = 256);

}  // namespace C2PA
