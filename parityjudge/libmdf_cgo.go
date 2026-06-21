package main

/*
#cgo CFLAGS: -I${SRCDIR}/../include -I${SRCDIR}/../src -D_POSIX_C_SOURCE=200809L
#include <stddef.h>
#include <stdlib.h>
#include "libmdf/mdf.h"

int libmdf_bridge_render(int format,
                         const char *data,
                         size_t len,
                         int chunk,
                         int width,
                         const char *theme,
                         int boring,
                         int osc8,
                         int table_buffer,
                         int table_wire,
                         int trace,
                         char **out,
                         size_t *out_len,
                         char **trace_out,
                         size_t *trace_len);
void libmdf_bridge_free(void *ptr);
*/
import "C"

import (
	"fmt"
	"unsafe"

	"pkt.systems/mdf"
)

func boolInt(v bool) C.int {
	if v {
		return 1
	}
	return 0
}

func cTableBuffer(mode mdf.TableBufferMode) C.int {
	switch mode {
	case mdf.TableBufferRow:
		return C.MDF_TABLE_BUFFER_ROW
	default:
		return C.MDF_TABLE_BUFFER_FULL
	}
}

func cTableWire(mode mdf.TableWireMode) C.int {
	switch mode {
	case mdf.TableWireASCII:
		return C.MDF_TABLE_WIRE_ASCII
	case mdf.TableWireSpace:
		return C.MDF_TABLE_WIRE_SPACE
	default:
		return C.MDF_TABLE_WIRE_LINE
	}
}

func renderLibmdf(mode string, data []byte, chunk int, width int, opts parityOptions, trace bool) ([]byte, []byte, error) {
	format := C.int(0)
	switch mode {
	case "ansi":
		format = 0
	case "html":
		if trace {
			return nil, nil, fmt.Errorf("libmdf HTML write trace is not supported")
		}
		format = 1
	default:
		return nil, nil, fmt.Errorf("libmdf comparison is not supported for mode %q", mode)
	}

	var in unsafe.Pointer
	if len(data) > 0 {
		in = C.CBytes(data)
		defer C.free(in)
	}

	var out *C.char
	var outLen C.size_t
	var traceOut *C.char
	var traceLen C.size_t
	theme := C.CString(opts.theme)
	defer C.free(unsafe.Pointer(theme))
	traceInt := C.int(0)
	if trace {
		traceInt = 1
	}
	status := C.libmdf_bridge_render(
		format,
		(*C.char)(in),
		C.size_t(len(data)),
		C.int(chunk),
		C.int(width),
		theme,
		boolInt(opts.boring),
		boolInt(opts.osc8),
		cTableBuffer(opts.tableBuffer),
		cTableWire(opts.tableWire),
		traceInt,
		&out,
		&outLen,
		&traceOut,
		&traceLen,
	)
	if status != 0 {
		return nil, nil, fmt.Errorf("libmdf render failed: status %d", int(status))
	}
	defer C.libmdf_bridge_free(unsafe.Pointer(out))
	defer C.libmdf_bridge_free(unsafe.Pointer(traceOut))

	rendered := C.GoBytes(unsafe.Pointer(out), C.int(outLen))
	traced := C.GoBytes(unsafe.Pointer(traceOut), C.int(traceLen))
	return rendered, traced, nil
}
