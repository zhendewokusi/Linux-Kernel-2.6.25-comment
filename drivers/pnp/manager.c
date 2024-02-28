/*
 * manager.c - Resource Management, Conflict Resolution, Activation and Disabling of Devices
 *
 * based on isapnp.c resource management (c) Jaroslav Kysela <perex@perex.cz>
 * Copyright 2003 Adam Belay <ambx1@neo.rr.com>
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pnp.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include <linux/mutex.h>
#include "base.h"

DEFINE_MUTEX(pnp_res_mutex);

static int pnp_assign_port(struct pnp_dev *dev, struct pnp_port *rule, int idx)
{
	resource_size_t *start, *end;
	unsigned long *flags;

	if (idx >= PNP_MAX_PORT) {
		dev_err(&dev->dev, "too many I/O port resources\n");
		/* pretend we were successful so at least the manager won't try again */
		return 1;
	}

	/* check if this resource has been manually set, if so skip */
	if (!(dev->res.port_resource[idx].flags & IORESOURCE_AUTO))
		return 1;

	start = &dev->res.port_resource[idx].start;
	end = &dev->res.port_resource[idx].end;
	flags = &dev->res.port_resource[idx].flags;

	/* set the initial values */
	*flags |= rule->flags | IORESOURCE_IO;
	*flags &= ~IORESOURCE_UNSET;

	if (!rule->size) {
		*flags |= IORESOURCE_DISABLED;
		return 1;	/* skip disabled resource requests */
	}

	*start = rule->min;
	*end = *start + rule->size - 1;

	/* run through until pnp_check_port is happy */
	while (!pnp_check_port(dev, idx)) {
		*start += rule->align;
		*end = *start + rule->size - 1;
		if (*start > rule->max || !rule->align)
			return 0;
	}
	return 1;
}

static int pnp_assign_mem(struct pnp_dev *dev, struct pnp_mem *rule, int idx)
{
	resource_size_t *start, *end;
	unsigned long *flags;

	if (idx >= PNP_MAX_MEM) {
		dev_err(&dev->dev, "too many memory resources\n");
		/* pretend we were successful so at least the manager won't try again */
		return 1;
	}

	/* check if this resource has been manually set, if so skip */
	if (!(dev->res.mem_resource[idx].flags & IORESOURCE_AUTO))
		return 1;

	start = &dev->res.mem_resource[idx].start;
	end = &dev->res.mem_resource[idx].end;
	flags = &dev->res.mem_resource[idx].flags;

	/* set the initial values */
	*flags |= rule->flags | IORESOURCE_MEM;
	*flags &= ~IORESOURCE_UNSET;

	/* convert pnp flags to standard Linux flags */
	if (!(rule->flags & IORESOURCE_MEM_WRITEABLE))
		*flags |= IORESOURCE_READONLY;
	if (rule->flags & IORESOURCE_MEM_CACHEABLE)
		*flags |= IORESOURCE_CACHEABLE;
	if (rule->flags & IORESOURCE_MEM_RANGELENGTH)
		*flags |= IORESOURCE_RANGELENGTH;
	if (rule->flags & IORESOURCE_MEM_SHADOWABLE)
		*flags |= IORESOURCE_SHADOWABLE;

	if (!rule->size) {
		*flags |= IORESOURCE_DISABLED;
		return 1;	/* skip disabled resource requests */
	}

	*start = rule->min;
	*end = *start + rule->size - 1;

	/* run through until pnp_check_mem is happy */
	while (!pnp_check_mem(dev, idx)) {
		*start += rule->align;
		*end = *start + rule->size - 1;
		if (*start > rule->max || !rule->align)
			return 0;
	}
	return 1;
}

static int pnp_assign_irq(struct pnp_dev *dev, struct pnp_irq *rule, int idx)
{
	resource_size_t *start, *end;
	unsigned long *flags;
	int i;

	/* IRQ priority: this table is good for i386 */
	static unsigned short xtab[16] = {
		5, 10, 11, 12, 9, 14, 15, 7, 3, 4, 13, 0, 1, 6, 8, 2
	};

	if (idx >= PNP_MAX_IRQ) {
		dev_err(&dev->dev, "too many IRQ resources\n");
		/* pretend we were successful so at least the manager won't try again */
		return 1;
	}

	/* check if this resource has been manually set, if so skip */
	if (!(dev->res.irq_resource[idx].flags & IORESOURCE_AUTO))
		return 1;

	start = &dev->res.irq_resource[idx].start;
	end = &dev->res.irq_resource[idx].end;
	flags = &dev->res.irq_resource[idx].flags;

	/* set the initial values */
	*flags |= rule->flags | IORESOURCE_IRQ;
	*flags &= ~IORESOURCE_UNSET;

	if (bitmap_empty(rule->map, PNP_IRQ_NR)) {
		*flags |= IORESOURCE_DISABLED;
		return 1;	/* skip disabled resource requests */
	}

	/* TBD: need check for >16 IRQ */
	*start = find_next_bit(rule->map, PNP_IRQ_NR, 16);
	if (*start < PNP_IRQ_NR) {
		*end = *start;
		return 1;
	}
	for (i = 0; i < 16; i++) {
		if (test_bit(xtab[i], rule->map)) {
			*start = *end = xtab[i];
			if (pnp_check_irq(dev, idx))
				return 1;
		}
	}
	return 0;
}

static void pnp_assign_dma(struct pnp_dev *dev, struct pnp_dma *rule, int idx)
{
	resource_size_t *start, *end;
	unsigned long *flags;
	int i;

	/* DMA priority: this table is good for i386 */
	static unsigned short xtab[8] = {
		1, 3, 5, 6, 7, 0, 2, 4
	};

	if (idx >= PNP_MAX_DMA) {
		dev_err(&dev->dev, "too many DMA resources\n");
		return;
	}

	/* check if this resource has been manually set, if so skip */
	if (!(dev->res.dma_resource[idx].flags & IORESOURCE_AUTO))
		return;

	start = &dev->res.dma_resource[idx].start;
	end = &dev->res.dma_resource[idx].end;
	flags = &dev->res.dma_resource[idx].flags;

	/* set the initial values */
	*flags |= rule->flags | IORESOURCE_DMA;
	*flags &= ~IORESOURCE_UNSET;

	for (i = 0; i < 8; i++) {
		if (rule->map & (1 << xtab[i])) {
			*start = *end = xtab[i];
			if (pnp_check_dma(dev, idx))
				return;
		}
	}
#ifdef MAX_DMA_CHANNELS
	*start = *end = MAX_DMA_CHANNELS;
#endif
	*flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
}

/**
 * pnp_init_resources - Resets a resource table to default values.
 * @table: pointer to the desired resource table
 */
void pnp_init_resource_table(struct pnp_resource_table *table)
{
	int idx;

	for (idx = 0; idx < PNP_MAX_IRQ; idx++) {
		table->irq_resource[idx].name = NULL;
		table->irq_resource[idx].start = -1;
		table->irq_resource[idx].end = -1;
		table->irq_resource[idx].flags =
		    IORESOURCE_IRQ | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
	for (idx = 0; idx < PNP_MAX_DMA; idx++) {
		table->dma_resource[idx].name = NULL;
		table->dma_resource[idx].start = -1;
		table->dma_resource[idx].end = -1;
		table->dma_resource[idx].flags =
		    IORESOURCE_DMA | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
	for (idx = 0; idx < PNP_MAX_PORT; idx++) {
		table->port_resource[idx].name = NULL;
		table->port_resource[idx].start = 0;
		table->port_resource[idx].end = 0;
		table->port_resource[idx].flags =
		    IORESOURCE_IO | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
	for (idx = 0; idx < PNP_MAX_MEM; idx++) {
		table->mem_resource[idx].name = NULL;
		table->mem_resource[idx].start = 0;
		table->mem_resource[idx].end = 0;
		table->mem_resource[idx].flags =
		    IORESOURCE_MEM | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
}

/**
 * pnp_clean_resources - clears resources that were not manually set
 * @res: the resources to clean
 */
static void pnp_clean_resource_table(struct pnp_resource_table *res)
{
	int idx;

	for (idx = 0; idx < PNP_MAX_IRQ; idx++) {
		if (!(res->irq_resource[idx].flags & IORESOURCE_AUTO))
			continue;
		res->irq_resource[idx].start = -1;
		res->irq_resource[idx].end = -1;
		res->irq_resource[idx].flags =
		    IORESOURCE_IRQ | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
	for (idx = 0; idx < PNP_MAX_DMA; idx++) {
		if (!(res->dma_resource[idx].flags & IORESOURCE_AUTO))
			continue;
		res->dma_resource[idx].start = -1;
		res->dma_resource[idx].end = -1;
		res->dma_resource[idx].flags =
		    IORESOURCE_DMA | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
	for (idx = 0; idx < PNP_MAX_PORT; idx++) {
		if (!(res->port_resource[idx].flags & IORESOURCE_AUTO))
			continue;
		res->port_resource[idx].start = 0;
		res->port_resource[idx].end = 0;
		res->port_resource[idx].flags =
		    IORESOURCE_IO | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
	for (idx = 0; idx < PNP_MAX_MEM; idx++) {
		if (!(res->mem_resource[idx].flags & IORESOURCE_AUTO))
			continue;
		res->mem_resource[idx].start = 0;
		res->mem_resource[idx].end = 0;
		res->mem_resource[idx].flags =
		    IORESOURCE_MEM | IORESOURCE_AUTO | IORESOURCE_UNSET;
	}
}

/**
 * pnp_assign_resources - assigns resources to the device based on the specified dependent number
 * @dev: pointer to the desired device
 * @depnum: the dependent function number
 *
 * Only set depnum to 0 if the device does not have dependent options.
 */
static int pnp_assign_resources(struct pnp_dev *dev, int depnum)
{
	struct pnp_port *port;
	struct pnp_mem *mem;
	struct pnp_irq *irq;
	struct pnp_dma *dma;
	int nport = 0, nmem = 0, nirq = 0, ndma = 0;

	if (!pnp_can_configure(dev))
		return -ENODEV;

	mutex_lock(&pnp_res_mutex);
	pnp_clean_resource_table(&dev->res);	/* start with a fresh slate */
	if (dev->independent) {
		port = dev->independent->port;
		mem = dev->independent->mem;
		irq = dev->independent->irq;
		dma = dev->independent->dma;
		while (port) {
			if (!pnp_assign_port(dev, port, nport))
				goto fail;
			nport++;
			port = port->next;
		}
		while (mem) {
			if (!pnp_assign_mem(dev, mem, nmem))
				goto fail;
			nmem++;
			mem = mem->next;
		}
		while (irq) {
			if (!pnp_assign_irq(dev, irq, nirq))
				goto fail;
			nirq++;
			irq = irq->next;
		}
		while (dma) {
			pnp_assign_dma(dev, dma, ndma);
			ndma++;
			dma = dma->next;
		}
	}

	if (depnum) {
		struct pnp_option *dep;
		int i;
		for (i = 1, dep = dev->dependent; i < depnum;
		     i++, dep = dep->next)
			if (!dep)
				goto fail;
		port = dep->port;
		mem = dep->mem;
		irq = dep->irq;
		dma = dep->dma;
		while (port) {
			if (!pnp_assign_port(dev, port, nport))
				goto fail;
			nport++;
			port = port->next;
		}
		while (mem) {
			if (!pnp_assign_mem(dev, mem, nmem))
				goto fail;
			nmem++;
			mem = mem->next;
		}
		while (irq) {
			if (!pnp_assign_irq(dev, irq, nirq))
				goto fail;
			nirq++;
			irq = irq->next;
		}
		while (dma) {
			pnp_assign_dma(dev, dma, ndma);
			ndma++;
			dma = dma->next;
		}
	} else if (dev->dependent)
		goto fail;

	mutex_unlock(&pnp_res_mutex);
	return 1;

fail:
	pnp_clean_resource_table(&dev->res);
	mutex_unlock(&pnp_res_mutex);
	return 0;
}

/**
 * pnp_manual_config_dev - Disables Auto Config and Manually sets the resource table
 * @dev: pointer to the desired device
 * @res: pointer to the new resource config
 * @mode: 0 or PNP_CONFIG_FORCE
 *
 * This function can be used by drivers that want to manually set thier resources.
 */
int pnp_manual_config_dev(struct pnp_dev *dev, struct pnp_resource_table *res,
			  int mode)
{
	int i;
	struct pnp_resource_table *bak;

	if (!pnp_can_configure(dev))
		return -ENODEV;
	bak = pnp_alloc(sizeof(struct pnp_resource_table));
	if (!bak)
		return -ENOMEM;
	*bak = dev->res;

	mutex_lock(&pnp_res_mutex);
	dev->res = *res;
	if (!(mode & PNP_CONFIG_FORCE)) {
		for (i = 0; i < PNP_MAX_PORT; i++) {
			if (!pnp_check_port(dev, i))
				goto fail;
		}
		for (i = 0; i < PNP_MAX_MEM; i++) {
			if (!pnp_check_mem(dev, i))
				goto fail;
		}
		for (i = 0; i < PNP_MAX_IRQ; i++) {
			if (!pnp_check_irq(dev, i))
				goto fail;
		}
		for (i = 0; i < PNP_MAX_DMA; i++) {
			if (!pnp_check_dma(dev, i))
				goto fail;
		}
	}
	mutex_unlock(&pnp_res_mutex);

	kfree(bak);
	return 0;

fail:
	dev->res = *bak;
	mutex_unlock(&pnp_res_mutex);
	kfree(bak);
	return -EINVAL;
}

/**
 * pnp_auto_config_dev - automatically assigns resources to a device
 * @dev: pointer to the desired device
 */
int pnp_auto_config_dev(struct pnp_dev *dev)
{
	struct pnp_option *dep;
	int i = 1;

	if (!pnp_can_configure(dev)) {
		dev_dbg(&dev->dev, "configuration not supported\n");
		return -ENODEV;
	}

	if (!dev->dependent) {
		if (pnp_assign_resources(dev, 0))
			return 0;
	} else {
		dep = dev->dependent;
		do {
			if (pnp_assign_resources(dev, i))
				return 0;
			dep = dep->next;
			i++;
		} while (dep);
	}

	dev_err(&dev->dev, "unable to assign resources\n");
	return -EBUSY;
}

/**
 * pnp_start_dev - low-level start of the PnP device
 * @dev: pointer to the desired device
 *
 * assumes that resources have already been allocated
 */
int pnp_start_dev(struct pnp_dev *dev)
{
	if (!pnp_can_write(dev)) {
		dev_dbg(&dev->dev, "activation not supported\n");
		return -EINVAL;
	}

	if (dev->protocol->set(dev, &dev->res) < 0) {
		dev_err(&dev->dev, "activation failed\n");
		return -EIO;
	}

	dev_info(&dev->dev, "activated\n");
	return 0;
}

/**
 * pnp_stop_dev - low-level disable of the PnP device
 * @dev: pointer to the desired device
 *
 * does not free resources
 */
int pnp_stop_dev(struct pnp_dev *dev)
{
	if (!pnp_can_disable(dev)) {
		dev_dbg(&dev->dev, "disabling not supported\n");
		return -EINVAL;
	}
	if (dev->protocol->disable(dev) < 0) {
		dev_err(&dev->dev, "disable failed\n");
		return -EIO;
	}

	dev_info(&dev->dev, "disabled\n");
	return 0;
}

/**
 * pnp_activate_dev - activates a PnP device for use
 * @dev: pointer to the desired device
 *
 * does not validate or set resources so be careful.
 */
int pnp_activate_dev(struct pnp_dev *dev)
{
	int error;

	if (dev->active)
		return 0;

	/* ensure resources are allocated */
	if (pnp_auto_config_dev(dev))
		return -EBUSY;

	error = pnp_start_dev(dev);
	if (error)
		return error;

	dev->active = 1;
	return 0;
}

/**
 * pnp_disable_dev - disables device
 * @dev: pointer to the desired device
 *
 * inform the correct pnp protocol so that resources can be used by other devices
 */
int pnp_disable_dev(struct pnp_dev *dev)
{
	int error;

	if (!dev->active)
		return 0;

	error = pnp_stop_dev(dev);
	if (error)
		return error;

	dev->active = 0;

	/* release the resources so that other devices can use them */
	mutex_lock(&pnp_res_mutex);
	pnp_clean_resource_table(&dev->res);
	mutex_unlock(&pnp_res_mutex);

	return 0;
}

/**
 * pnp_resource_change - change one resource
 * @resource: pointer to resource to be changed
 * @start: start of region
 * @size: size of region
 */
void pnp_resource_change(struct resource *resource, resource_size_t start,
			 resource_size_t size)
{
	resource->flags &= ~(IORESOURCE_AUTO | IORESOURCE_UNSET);
	resource->start = start;
	resource->end = start + size - 1;
}

EXPORT_SYMBOL(pnp_manual_config_dev);
EXPORT_SYMBOL(pnp_start_dev);
EXPORT_SYMBOL(pnp_stop_dev);
EXPORT_SYMBOL(pnp_activate_dev);
EXPORT_SYMBOL(pnp_disable_dev);
EXPORT_SYMBOL(pnp_resource_change);
EXPORT_SYMBOL(pnp_init_resource_table);
