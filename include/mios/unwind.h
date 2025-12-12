#pragma once

struct stream;
struct thread;

void backtrace_print(struct stream *st);

void backtrace_print_thread(struct stream *st, struct thread *t);

void backtrace_print_frame(struct stream *st, void *frame);
