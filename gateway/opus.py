"""Mono Opus frames used by the device WebSocket protocol."""
import ctypes as c
from ctypes.util import find_library
import os
import sys

if sys.byteorder != "little":
    raise RuntimeError("16-bit PCM requires a little-endian gateway host")

lib = c.CDLL(os.getenv("OMNI_OPUS_LIBRARY") or find_library("opus") or "libopus.so.0")
samples = c.POINTER(c.c_int16)


def bind(name, result, *args):
    fn = getattr(lib, name)
    fn.argtypes = args
    fn.restype = result
    return fn


new_decoder = bind("opus_decoder_create", c.c_void_p, c.c_int, c.c_int, c.POINTER(c.c_int))
new_encoder = bind("opus_encoder_create", c.c_void_p, c.c_int, c.c_int, c.c_int, c.POINTER(c.c_int))
free_decoder = bind("opus_decoder_destroy", None, c.c_void_p)
free_encoder = bind("opus_encoder_destroy", None, c.c_void_p)
decode = bind("opus_decode", c.c_int, c.c_void_p, c.c_char_p, c.c_int, samples, c.c_int, c.c_int)
encode = bind("opus_encode", c.c_int, c.c_void_p, samples, c.c_int, c.c_void_p, c.c_int)


class AudioCodec:
    def __init__(self):
        err = c.c_int()
        self.decoder = new_decoder(16000, 1, c.byref(err))
        if not self.decoder:
            raise RuntimeError("Opus input allocation failed")
        self.encoder = new_encoder(24000, 1, 2048, c.byref(err))
        if not self.encoder:
            free_decoder(self.decoder)
            raise RuntimeError("Opus output allocation failed")

    def close(self):
        free_decoder(self.decoder)
        free_encoder(self.encoder)

    def pcm(self, packet):
        frame = (c.c_int16 * 960)()
        n = decode(self.decoder, packet, len(packet), frame, len(frame), 0)
        if n != len(frame):
            raise ValueError("expected one 60 ms Opus packet")
        return bytes(frame)

    def packet(self, pcm):
        frame = (c.c_int16 * 1440).from_buffer_copy(pcm)
        output = c.create_string_buffer(1275)
        n = encode(self.encoder, frame, len(frame), output, len(output))
        if n <= 0:
            raise ValueError("Opus output encoding failed")
        return output.raw[:n]
