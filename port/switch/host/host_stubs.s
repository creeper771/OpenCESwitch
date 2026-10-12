/*
HOST_STUBS.S

A stub for each import the Switch host does not implement yet: stub n
calls host_missing_import_called(n) (host_imports.c), which logs the
import's name the first time and returns 0 to the guest.
*/

	.text
	.global host_import_stubs
	.balign 16
host_import_stubs:
	.set stub_index, 0
	.rept 512
	mov w0, #stub_index
	b host_missing_import_called
	.set stub_index, stub_index + 1
	.endr
