#pragma once

struct stream;

// Pipe creates two streams that are connected
// This is the opposite of splice() which connects two arbitrary streams

void pipe(struct stream **a, struct stream **b);
