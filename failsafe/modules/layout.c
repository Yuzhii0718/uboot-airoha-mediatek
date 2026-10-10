// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718 <admin@yuzhii0718.eu.org>
 *
 * Flash layouts of the board device tree - see <failsafe/layout.h>.
 *
 * The node is read from the control device tree, the same tree the strict
 * model check uses (see failsafe/bootimg/image.c): it describes the flash
 * the running board has, not the one of an uploaded image.
 */

#include <asm/global_data.h>
#include <errno.h>
#include <image.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <failsafe/layout.h>
#include <cprint.h>

DECLARE_GLOBAL_DATA_PTR;

/* Decode @cells big-endian cells into a 64-bit value. */
static u64 failsafe_layout_cells(const fdt32_t *cell, int cells)
{
	u64 value = 0;
	int i;

	for (i = 0; i < cells; i++)
		value = (value << 32) | fdt32_to_cpu(cell[i]);

	return value;
}

/*
 * Read the partitions of one layout.
 *
 * "reg" is decoded with the #address-cells / #size-cells in effect for the
 * partitions: a layout node may declare them itself, and so may the node that
 * holds the layouts - which is where the binding documented in
 * <failsafe/layout.h> puts them.  A board that describes its layouts with a
 * plainer "<offset size>" pair than those cells allow is still understood: a
 * two-cell "reg" is then read as an offset and a size, with a note on the
 * console.
 */
static void failsafe_layout_parse_parts(const void *fdt, int layout_node,
					int layouts_node,
					struct failsafe_layout *layout)
{
	int part_node;

	for (part_node = fdt_first_subnode(fdt, layout_node);
	     part_node >= 0 && layout->num_parts < FAILSAFE_LAYOUT_MAX_PARTS;
	     part_node = fdt_next_subnode(fdt, part_node)) {
		const char *label;
		const fdt32_t *reg;
		int len, addr_cells, size_cells, cells_node;
		u64 offset, size;

		label = fdt_getprop(fdt, part_node,
				    FAILSAFE_LAYOUT_PROP_LABEL, NULL);
		if (!label || !*label)
			continue;

		reg = fdt_getprop(fdt, part_node, FAILSAFE_LAYOUT_PROP_REG,
				  &len);
		if (!reg) {
			cprintln(CAUTION, "Failsafe: layout partition '%s' has "
				 "no reg, skipped", label);
			continue;
		}

		/*
		 * The cells of the partitions' "reg" come from the layout
		 * node when it declares them, and from the node holding the
		 * layouts otherwise (see <failsafe/layout.h>); libfdt's own
		 * defaults apply when neither does, as for any other node.
		 */
		cells_node = layout_node;
		if (!fdt_getprop(fdt, cells_node, "#address-cells", NULL) &&
		    fdt_getprop(fdt, layouts_node, "#address-cells", NULL))
			cells_node = layouts_node;

		addr_cells = fdt_address_cells(fdt, cells_node);
		if (addr_cells < 1 || addr_cells > 2)
			addr_cells = 1;

		cells_node = layout_node;
		if (!fdt_getprop(fdt, cells_node, "#size-cells", NULL) &&
		    fdt_getprop(fdt, layouts_node, "#size-cells", NULL))
			cells_node = layouts_node;

		size_cells = fdt_size_cells(fdt, cells_node);
		if (size_cells < 1 || size_cells > 2)
			size_cells = 1;

		if (len == (addr_cells + size_cells) * (int)sizeof(fdt32_t)) {
			offset = failsafe_layout_cells(reg, addr_cells);
			size = failsafe_layout_cells(reg + addr_cells,
						     size_cells);
		} else if (len == 2 * (int)sizeof(fdt32_t)) {
			cprintln(CAUTION, "Failsafe: layout partition '%s': "
				 "reg does not match #address-cells/"
				 "#size-cells, read as <offset size>",
				 label);
			offset = failsafe_layout_cells(reg, 1);
			size = failsafe_layout_cells(reg + 1, 1);
		} else {
			cprintln(CAUTION, "Failsafe: layout partition '%s': "
				 "reg has %d bytes, skipped", label, len);
			continue;
		}

		layout->parts[layout->num_parts].label = label;
		layout->parts[layout->num_parts].offset = offset;
		layout->parts[layout->num_parts].size = size;
		layout->num_parts++;
	}
}

int failsafe_layout_parse(struct failsafe_layout *layouts, int max)
{
	const void *fdt = gd_fdt_blob();
	int node, layout_node, count = 0;

	if (!layouts || max <= 0)
		return 0;

	if (!fdt || fdt_check_header(fdt))
		return 0;

	node = fdt_node_offset_by_compatible(fdt, -1,
					     FAILSAFE_LAYOUT_COMPATIBLE);
	if (node < 0)
		return 0;

	for (layout_node = fdt_first_subnode(fdt, node);
	     layout_node >= 0 && count < max;
	     layout_node = fdt_next_subnode(fdt, layout_node)) {
		const char *label;

		label = fdt_getprop(fdt, layout_node,
				    FAILSAFE_LAYOUT_PROP_LABEL, NULL);
		if (!label || !*label)
			continue;

		memset(&layouts[count], 0, sizeof(layouts[count]));
		layouts[count].label = label;

		failsafe_layout_parse_parts(fdt, layout_node, node,
					    &layouts[count]);
		if (!layouts[count].num_parts) {
			cprintln(CAUTION, "Failsafe: layout '%s' has no usable "
				 "partition, skipped", label);
			continue;
		}

		count++;
	}

	/* More layouts than the caller can hold: say so rather than dropping
	 * them silently. */
	for (; layout_node >= 0;
	     layout_node = fdt_next_subnode(fdt, layout_node)) {
		if (fdt_getprop(fdt, layout_node, FAILSAFE_LAYOUT_PROP_LABEL,
				NULL)) {
			cprintln(CAUTION, "Failsafe: more than %d flash "
				 "layouts in the device tree, the rest is "
				 "ignored", max);
			break;
		}
	}

	return count;
}

const struct failsafe_layout_part *failsafe_layout_find_part(
			const struct failsafe_layout *layout, const char *label)
{
	int i;

	if (!layout || !label)
		return NULL;

	for (i = 0; i < layout->num_parts; i++) {
		if (!strcmp(layout->parts[i].label, label))
			return &layout->parts[i];
	}

	return NULL;
}
