package main

import (
	"image"
	"image/color"
	"testing"
)

// encodeBytes mirrors encode_frame_stamp_bytes() in
// engine/include/orchestration/frame_stamp.h so the test drives the decoder
// against the exact byte/checksum layout the engine burns in.
func encodeBytes(seq uint64) [stampRows]uint8 {
	var out [stampRows]uint8
	for i := 0; i < stampSeqRows; i++ {
		out[i] = uint8((seq >> (56 - 8*i)) & 0xFF)
	}
	var cs uint8
	for i := 0; i < stampSeqRows; i++ {
		cs ^= out[i]
	}
	out[stampSeqRows] = cs
	return out
}

// drawStamp renders the scaled, bottom-anchored bit grid for seq onto a w x h
// image, matching draw_frame_stamp() in inference_worker.cpp.
func drawStamp(seq uint64, w, h int) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	// Fill mid-gray so a decode that reads the wrong region would fail loudly.
	gray := color.RGBA{128, 128, 128, 255}
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.Set(x, y, gray)
		}
	}
	cell := stampDim(stampCellPx, h)
	gridX0 := stampDim(stampMargin, h) + cell
	gridY0 := h - stampDim(stampTextBand, h) - (stampRows+1)*cell
	bytes := encodeBytes(seq)
	for row := 0; row < stampRows; row++ {
		for col := 0; col < stampCols; col++ {
			c := color.RGBA{0, 0, 0, 255}
			if (bytes[row]>>(7-col))&1 == 1 {
				c = color.RGBA{255, 255, 255, 255}
			}
			fillCell(img, gridX0, gridY0, cell, row, col, c)
		}
	}
	return img
}

func fillCell(img *image.RGBA, gridX0, gridY0, cell, row, col int, c color.RGBA) {
	x0 := gridX0 + col*cell
	y0 := gridY0 + row*cell
	for y := y0; y < y0+cell; y++ {
		for x := x0; x < x0+cell; x++ {
			img.Set(x, y, c)
		}
	}
}

func TestStampDim(t *testing.T) {
	// cell = max(10, round(10*H/600)); floored at the base for low resolutions.
	for h, want := range map[int]int{240: 10, 480: 10, 576: 10, 720: 12, 1024: 17, 1080: 18, 1920: 32} {
		if got := stampDim(stampCellPx, h); got != want {
			t.Errorf("stampDim(cell,%d)=%d, want %d", h, got, want)
		}
	}
}

func TestDecodeRoundTrip(t *testing.T) {
	seqs := []uint64{0, 1, 42, 255, 256, 65535, uint64(1) << 32, 1234567890123, ^uint64(0)}
	for _, want := range seqs {
		// Non-square, scale 1 (h=240).
		img := drawStamp(want, 320, 240)
		got, err := decodeFrameSeq(img)
		if err != nil {
			t.Fatalf("seq %d: decode error: %v", want, err)
		}
		if got != want {
			t.Fatalf("seq %d: decoded %d", want, got)
		}
	}
}

func TestDecodeRoundTripHighRes(t *testing.T) {
	// 1920 tall -> cell 32; confirms the decoder tracks the continuous scale.
	if got := stampDim(stampCellPx, 1920); got != 32 {
		t.Fatalf("cell@1920=%d, want 32", got)
	}
	for _, want := range []uint64{0, 1, 0xDEADBEEFCAFEF00D, ^uint64(0)} {
		img := drawStamp(want, 2560, 1920)
		got, err := decodeFrameSeq(img)
		if err != nil {
			t.Fatalf("seq %d @2560x1920: %v", want, err)
		}
		if got != want {
			t.Fatalf("seq %d @2560x1920: decoded %d", want, got)
		}
	}
}

func TestDecodeChecksumMismatch(t *testing.T) {
	const w, h = 320, 240
	img := drawStamp(0x0102030405060708, w, h)
	cell := stampDim(stampCellPx, h)
	gridX0 := stampDim(stampMargin, h) + cell
	gridY0 := h - stampDim(stampTextBand, h) - (stampRows+1)*cell
	off := cell / 2
	// Flip cell [0,0] without updating the checksum row -> mismatch.
	if luma(img, gridX0+off, gridY0+off) > 127 {
		fillCell(img, gridX0, gridY0, cell, 0, 0, color.RGBA{0, 0, 0, 255})
	} else {
		fillCell(img, gridX0, gridY0, cell, 0, 0, color.RGBA{255, 255, 255, 255})
	}
	if _, err := decodeFrameSeq(img); err == nil {
		t.Fatal("expected checksum mismatch error, got nil")
	}
}

func TestDecodeTooSmall(t *testing.T) {
	if _, err := decodeFrameSeq(image.NewRGBA(image.Rect(0, 0, 50, 50))); err == nil {
		t.Fatal("expected too-small error, got nil")
	}
}
