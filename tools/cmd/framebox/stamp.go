package main

import (
	"fmt"
	"image"
)

// Machine-readable frame-stamp geometry. These MUST match the burn-in in
// engine/src/orchestration/inference_worker.cpp (draw_frame_stamp) and the
// byte/checksum encoding in engine/include/orchestration/frame_stamp.h.
//
// The stamp scales continuously with frame height H and is bottom-anchored
// (lower-left corner) above a reserved text band, so its geometry depends on H.
// These constants are the base sizes; each is scaled as
// scaled(base) = max(base, round(base*H/stampScaleRef)). Grid cell [0,0]'s
// top-left is at (margin + cell, H - text_band - (stampRows+1)*cell), where
// cell = scaled(stampCellPx), margin = scaled(stampMargin),
// text_band = scaled(stampTextBand).
const (
	stampCellPx   = 10  // base (1x) cell size
	stampMargin   = 4   // base (1x) margin
	stampTextBand = 28  // base (1x) bottom text band
	stampCols     = 8   // bits per row (one byte, MSB first)
	stampRows     = 9   // rows 0..7 = seq bytes MSB-first, row 8 = XOR checksum
	stampSeqRows  = 8   // number of rows holding seq bytes
	stampScaleRef = 600 // frame-height reference for the integer scale factor
)

// stampDim mirrors the engine's scaled(base) = max(base, round(base*H/stampScaleRef)),
// using the same integer round-half-up as the engine.
func stampDim(base, h int) int {
	v := (h*base + stampScaleRef/2) / stampScaleRef
	if v < base {
		v = base
	}
	return v
}

// decodeFrameSeq reads the 8x9 bit grid burned into the lower-left of an
// annotated frame and returns the encoded frame_seq. It returns an error when
// the frame is too small to contain the grid or the XOR checksum row does not
// match the decoded bytes (blur, cropping, heavy recompression, or no stamp).
func decodeFrameSeq(img image.Image) (uint64, error) {
	b := img.Bounds()
	h := b.Dy()
	cell := stampDim(stampCellPx, h)
	margin := stampDim(stampMargin, h)
	textBand := stampDim(stampTextBand, h)
	gridX0 := margin + cell
	gridY0 := h - textBand - (stampRows+1)*cell // bottom-anchored, above the text band
	off := cell / 2                             // cell-center offset

	if gridY0 < 0 {
		return 0, fmt.Errorf("image %dx%d too short to contain the frame stamp", b.Dx(), b.Dy())
	}
	maxX := gridX0 + (stampCols-1)*cell + off
	if b.Dx() <= maxX {
		return 0, fmt.Errorf("image %dx%d too narrow to contain the frame stamp (need width > %d)", b.Dx(), b.Dy(), maxX)
	}

	var rowBytes [stampRows]uint8
	for row := 0; row < stampRows; row++ {
		var v uint8
		for col := 0; col < stampCols; col++ {
			x := b.Min.X + gridX0 + col*cell + off
			y := b.Min.Y + gridY0 + row*cell + off
			v <<= 1
			if luma(img, x, y) > 127 {
				v |= 1 // white = bit 1
			}
		}
		rowBytes[row] = v
	}

	var checksum uint8
	for row := 0; row < stampSeqRows; row++ {
		checksum ^= rowBytes[row]
	}
	if checksum != rowBytes[stampSeqRows] {
		return 0, fmt.Errorf("frame-stamp checksum mismatch (got 0x%02x, want 0x%02x): unreadable or unstamped frame",
			rowBytes[stampSeqRows], checksum)
	}

	var seq uint64
	for row := 0; row < stampSeqRows; row++ {
		seq = seq<<8 | uint64(rowBytes[row])
	}
	return seq, nil
}

// luma returns the 8-bit Rec.601 luma at (x,y). Stamp cells are pure black or
// white, so the exact weighting is not critical -- only the black/white split.
func luma(img image.Image, x, y int) uint8 {
	r, g, bl, _ := img.At(x, y).RGBA() // 16-bit per channel
	r8, g8, b8 := r>>8, g>>8, bl>>8
	return uint8((299*r8 + 587*g8 + 114*b8) / 1000)
}
