#pragma once

struct stream;

void pipe_bidir(struct stream *a, struct stream *b, const char *name);

