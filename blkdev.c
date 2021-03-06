#include <linux/fs.h>
#include <linux/ata.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/libata.h>
#include <linux/module.h>
#include <linux/kobject.h>

#define TA_MODULE_NAME  "aeroDisk"

static struct kset *bdev_kset;

struct ta_aero_dev {
	struct kobject  kobj;
	u16            *id;
	u64             wwn;
	u64             disk_sizeb;
};

struct ta_attribute {
	struct attribute attr;
	ssize_t (*show)(struct ta_aero_dev *aero, struct ta_attribute *attr,
	                char *buf);
	ssize_t (*store)(struct ta_aero_dev *aero, struct ta_attribute *attr,
	                 const char *buf, size_t count);
};

#define ta_blkdev_log(...)    \
	pr_info(TA_MODULE_NAME ": " __VA_ARGS__)
#define to_aero_attr(ta_attr) \
	container_of(ta_attr, struct ta_attribute, attr)
#define kobj_to_aero(aero)    \
	container_of(aero, struct ta_aero_dev, kobj)

static bool __must_check ta_pci_dev_is_real_blkdev(u16 data)
{
	switch (data) {
	case PCI_CLASS_STORAGE_IDE:
	case PCI_CLASS_STORAGE_SCSI:
	case PCI_CLASS_STORAGE_SATA:
	case PCI_CLASS_STORAGE_RAID:
		return true;
	default:
		return false;
	}
}

static ssize_t ta_size_show(struct ta_aero_dev *aero,
                            struct ta_attribute *attr, char *buf)
{
	if (unlikely(!aero))
		return -EIO;

	return sprintf(buf, "%llu\n", aero->disk_sizeb);
}

static ssize_t ta_disk_id_show(struct ta_aero_dev *aero,
                               struct ta_attribute *attr, char *buf)
{
	if (unlikely(!aero))
		return -EIO;

	return sprintf(buf, "0x%llx\n", aero->wwn);
}

static void ta_aero_release(struct kobject *kobj)
{
	struct ta_aero_dev *aero;

	aero = kobj_to_aero(kobj);
	kfree(aero);
}

static struct ta_attribute size_attribute =
	__ATTR(size, S_IRUGO | S_IWUSR, ta_size_show, NULL);
static struct ta_attribute disk_id_attribute =
	__ATTR(disk_id, S_IRUGO | S_IWUSR, ta_disk_id_show, NULL);

static struct attribute *aero_default_attrs[] = {
	&size_attribute.attr,
	&disk_id_attribute.attr,
	NULL,
};

static ssize_t ta_aero_attr_show(struct kobject *kobj,
                                 struct attribute *attr, char *buf)
{
	struct ta_aero_dev *aero;
	struct ta_attribute *attribute;

	attribute = to_aero_attr(attr);
	aero = kobj_to_aero(kobj);

	if (unlikely(!attribute->show))
		return -EIO;

	return attribute->show(aero, attribute, buf);
}

static const struct sysfs_ops aero_sysfs_ops = {
	.show = ta_aero_attr_show,
};

static struct kobj_type aero_ktype = {
	.sysfs_ops = &aero_sysfs_ops,
	.release = ta_aero_release,
	.default_attrs = aero_default_attrs,
};

static void ta_destroy_aero_dev(struct ta_aero_dev *aero)
{
	kobject_put(&aero->kobj);
}

static struct ta_aero_dev __must_check *ta_create_adev(struct pci_dev *pdev,
                                                       struct kset *bdev_kset)
{
	int i, err;
	struct ata_port *ap;
	struct ata_host *host;
	struct ata_device *tdev;
	struct ta_aero_dev *aero;
	unsigned char buf[ATA_ID_WWN_LEN];

	aero = kzalloc(sizeof(*aero), GFP_KERNEL);
	if (unlikely(!aero))
		return NULL;

	aero->kobj.kset = bdev_kset;
	host = pci_get_drvdata(pdev);
	if (unlikely(!host)) {
		kobject_put(&aero->kobj);
		return NULL;
	}

	for (i = 0; i < host->n_ports; ++i) {
		ap = host->ports[i];
		ata_for_each_dev(tdev, &ap->link, ENABLED) {
			if (ata_id_has_wwn(tdev->id)) {
				aero->id = tdev->id;
				aero->disk_sizeb +=
					tdev->n_sectors * ATA_SECT_SIZE;
				ata_id_string(tdev->id, buf,
				              ATA_ID_WWN, ATA_ID_WWN_LEN);
				aero->wwn = be64_to_cpu(*((__force __be64 *)buf));
			}
		}
	}
	err = kobject_init_and_add(&aero->kobj, &aero_ktype,
	                           NULL, "%s", dev_name(&pdev->dev));
	if (unlikely(err)) {
		kobject_put(&aero->kobj);
		return NULL;
	}
	kobject_uevent(&aero->kobj, KOBJ_ADD);

	ta_blkdev_log("WWN: 0x%llx, disk size in bytes=%llu\n",
	              aero->wwn, aero->disk_sizeb);

	return aero;
}

static void __cold ta_destroy_kset(struct kset *kset)
{
	struct kobject *kobj;
	struct list_head *list_node, *list_tmp;

	if (likely(kset)) {
		list_for_each_safe(list_node, list_tmp, &kset->list) {
			kobj = container_of(list_node, struct kobject, entry);
			ta_destroy_aero_dev(kobj_to_aero(kobj));
		}
		kset_unregister(kset);
	}
}

static int __init ta_blkdev_init(void)
{
	u16 data;
	struct pci_dev *pdev = NULL;

	bdev_kset = kset_create_and_add(TA_MODULE_NAME, NULL, kernel_kobj);
	if (unlikely(!bdev_kset))
		return -ENOMEM;

	for_each_pci_dev(pdev) {
		pci_read_config_word(pdev, PCI_CLASS_DEVICE, &data);
		if (ta_pci_dev_is_real_blkdev(data)) {
			if (unlikely(!ta_create_adev(pdev, bdev_kset))) {
				ta_destroy_kset(bdev_kset);
				return -EINVAL;
			}
		}
	}
	ta_blkdev_log("loading\n");

	return 0;
}

static void __exit ta_blkdev_exit(void)
{
	ta_destroy_kset(bdev_kset);

	ta_blkdev_log("unloading\n");
}

module_init(ta_blkdev_init);
module_exit(ta_blkdev_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Testing ask aeroDisk");
MODULE_AUTHOR("Anton Mikaev ve_cni_k@inbox.ru");
MODULE_VERSION("0.02");
