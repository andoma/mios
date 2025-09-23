#pragma once

#include <string.h>
#include <sys/queue.h>
#include <stdint.h>

typedef struct {
  void *buffer;
  size_t capacity; // Size of allocation, may be larger than fdt.totalsize
} fdt_t;

typedef uint32_t fdt_node_ref_t;

/**
 * Return NULL if fdt seems OK
 * Otherwise a (compile time) constant string with the error
 */
const char *fdt_validate(const fdt_t *fdt);

/**
 * Given an fdt, return the next node matching @name
 * Name may contain wildcards between slashes denoted by an asterix '*'.
 *
 *    /cpus/\* ->  /cpus/cpu20100         <- first match
 *                 /cpus/cpu20200         <- second match
 *
 *    /cpus   ->  /cpus
 *
 * @prev is passed in from previous match to keep searching
 * the tree
 *
 * @match (if non-NULL) will be written with the "deepst" wildcard match
 *
 * The return value "fdt_node_ref_t" is a byte offset from start of
 * the tree.
 *
 *
 * Return value of 0 means no (more) match.
 *
 */
fdt_node_ref_t fdt_find_next_node(const fdt_t *fdt, fdt_node_ref_t prev,
                                  const char *name, const char **match);


/**
 * Overwrite a node with FDT_NOP until its end.
 */
void fdt_erase_node(const fdt_t *fdt, fdt_node_ref_t key);

/**
 * Return pointer to the property payload for a given node
 * NULL if property doesn't exist.

 * @lenp is filled with the byte length of the proprty
 */
const void *fdt_get_property(const fdt_t *fdt, fdt_node_ref_t key,
                             const char *name, size_t *lenp);

/**
 * Overwrite or add a property. The fdt will be expanded if necessary
 * Possible even reallocated
 *
 * @data contains the new proprty value.
 * @len is the length in bytes
 *
 * Returns non-zero if memory-(re)allocation failed
 */
int fdt_set_property(fdt_t *fdt, fdt_node_ref_t key, const char *name,
                     const void *data, size_t len);


uint32_t fdt_get_totalsize(const fdt_t *fdt);
