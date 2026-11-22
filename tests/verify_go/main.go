package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"os"

	"github.com/klauspost/compress/zstd"
	"github.com/pierrec/lz4/v4"
)

const (
	pacLZ4  = 1
	pacZSTD = 2
)

func bitshufDecodeBlock(out, in []byte, elemSize int) {
	n := len(in) / elemSize
	planeBytes := n / 8
	for i := range out {
		out[i] = 0
	}
	for p := 0; p < 8*elemSize; p++ {
		byteOff := p / 8
		bitInElem := byte(1) << uint(p%8)
		plane := in[p*planeBytes : (p+1)*planeBytes]
		for j := 0; j < planeBytes; j++ {
			b := plane[j]
			if b == 0 {
				continue
			}
			for k := 0; k < 8; k++ {
				if (b & (byte(1) << uint(k))) != 0 {
					elemIdx := j*8 + k
					out[elemIdx*elemSize+byteOff] |= bitInElem
				}
			}
		}
	}
}

func readU32LE(r io.Reader) (uint32, error) {
	var b [4]byte
	if _, err := io.ReadFull(r, b[:]); err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint32(b[:]), nil
}

func verifyCase(dec *zstd.Decoder, name string, codec, elem, hdrBlock uint32, raw, chunk []byte) error {
	if len(chunk) < 12 {
		return fmt.Errorf("%s: chunk < 12 bytes", name)
	}
	total := binary.BigEndian.Uint64(chunk[0:8])
	block := uint64(binary.BigEndian.Uint32(chunk[8:12]))
	if block == 0 {
		block = (8192 / uint64(elem) / 8) * 8 * uint64(elem)
	}
	if total != uint64(len(raw)) || block != uint64(hdrBlock) {
		return fmt.Errorf("%s: header mismatch total=%d raw=%d block=%d hdr=%d", name, total, len(raw), block, hdrBlock)
	}
	if total == 0 {
		if len(chunk) != 12 {
			return fmt.Errorf("%s: empty chunk len %d != 12", name, len(chunk))
		}
		return nil
	}

	out := make([]byte, len(raw))
	tmp := make([]byte, block)
	pos := 12
	var done uint64

	for done < total {
		b := total - done
		if b > block {
			b = block
		}
		left := b % (8 * uint64(elem))
		nb := b - left
		if nb > 0 {
			if pos+4 > len(chunk) {
				return fmt.Errorf("%s: truncated block size", name)
			}
			cs := int(binary.BigEndian.Uint32(chunk[pos : pos+4]))
			pos += 4
			if pos+cs > len(chunk) {
				return fmt.Errorf("%s: truncated block payload", name)
			}
			payload := chunk[pos : pos+cs]
			pos += cs
			if codec == pacZSTD {
				got, err := dec.DecodeAll(payload, tmp[:0])
				if err != nil {
					return fmt.Errorf("%s: klauspost zstd decode: %w", name, err)
				}
				if uint64(len(got)) != nb {
					return fmt.Errorf("%s: klauspost zstd len %d != %d", name, len(got), nb)
				}
				bitshufDecodeBlock(out[done:done+nb], got, int(elem))
			} else if codec == pacLZ4 {
				n, err := lz4.UncompressBlock(payload, tmp[:nb])
				if err != nil {
					return fmt.Errorf("%s: pierrec lz4 decode: %w", name, err)
				}
				if uint64(n) != nb {
					return fmt.Errorf("%s: pierrec lz4 len %d != %d", name, n, nb)
				}
				bitshufDecodeBlock(out[done:done+nb], tmp[:nb], int(elem))
			} else {
				return fmt.Errorf("%s: unknown codec %d", name, codec)
			}
		}
		if left > 0 {
			if pos+int(left) > len(chunk) {
				return fmt.Errorf("%s: truncated tail", name)
			}
			copy(out[done+nb:done+nb+left], chunk[pos:pos+int(left)])
			pos += int(left)
		}
		done += b
	}
	if pos != len(chunk) {
		return fmt.Errorf("%s: trailing bytes %d != %d", name, pos, len(chunk))
	}
	if !bytes.Equal(out, raw) {
		return fmt.Errorf("%s: decoded output mismatch", name)
	}
	return nil
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "usage: verify_go <compat_corpus.bin>\n")
		os.Exit(1)
	}
	f, err := os.Open(os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "open: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	var magic [8]byte
	if _, err := io.ReadFull(f, magic[:]); err != nil || string(magic[:]) != "PACC0001" {
		fmt.Fprintf(os.Stderr, "bad corpus magic\n")
		os.Exit(1)
	}
	count, err := readU32LE(f)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read count: %v\n", err)
		os.Exit(1)
	}

	dec, err := zstd.NewReader(nil, zstd.WithDecoderConcurrency(1), zstd.WithDecoderMaxWindow(128<<20))
	if err != nil {
		fmt.Fprintf(os.Stderr, "zstd.NewReader: %v\n", err)
		os.Exit(1)
	}
	defer dec.Close()

	failures := 0
	for i := uint32(0); i < count; i++ {
		nameLen, err := readU32LE(f)
		if err != nil {
			fmt.Fprintf(os.Stderr, "read record %d: %v\n", i, err)
			os.Exit(1)
		}
		codec, _ := readU32LE(f)
		elem, _ := readU32LE(f)
		block, _ := readU32LE(f)
		rawSize, _ := readU32LE(f)
		chunkSize, err := readU32LE(f)
		if err != nil {
			fmt.Fprintf(os.Stderr, "read header %d: %v\n", i, err)
			os.Exit(1)
		}
		nameBuf := make([]byte, nameLen)
		raw := make([]byte, rawSize)
		chunk := make([]byte, chunkSize)
		if _, err := io.ReadFull(f, nameBuf); err != nil {
			fmt.Fprintf(os.Stderr, "read name %d: %v\n", i, err)
			os.Exit(1)
		}
		if _, err := io.ReadFull(f, raw); err != nil {
			fmt.Fprintf(os.Stderr, "read raw %s: %v\n", string(nameBuf), err)
			os.Exit(1)
		}
		if _, err := io.ReadFull(f, chunk); err != nil {
			fmt.Fprintf(os.Stderr, "read chunk %s: %v\n", string(nameBuf), err)
			os.Exit(1)
		}
		if err := verifyCase(dec, string(nameBuf), codec, elem, block, raw, chunk); err != nil {
			fmt.Fprintf(os.Stderr, "FAIL %v\n", err)
			failures++
		}
	}
	fmt.Printf("go (klauspost/zstd + pierrec/lz4): %d cases checked, %d failures\n", count, failures)
	if failures > 0 {
		os.Exit(1)
	}
}
