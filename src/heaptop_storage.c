/**
 * @file heaptop_storage.c
 * @brief Flash chip, partition table and NVS facts, read on demand into a heaptop_storage_t.
 */

#include <string.h>

#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "heaptop.h"
#include "nvs.h"

/* NVS stores everything in 32-byte entries (the page format of nvs_flash; there is no public constant for it). */
#define HEAPTOP_NVS_ENTRY_BYTES 32

static void _read_partitions(heaptop_storage_t *out)
{
  const esp_partition_t *running = esp_ota_get_running_partition();

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL)
  {
    if (out->partition_count >= HEAPTOP_STORAGE_MAX_PARTITIONS)
    {
      out->partitions_truncated = true;
      break;
    }

    const esp_partition_t *p = esp_partition_get(it);
    heaptop_partition_t *row = &out->partitions[out->partition_count++];
    strlcpy(row->label, p->label, sizeof(row->label));
    row->type = (uint8_t)p->type;
    row->subtype = (uint8_t)p->subtype;
    row->encrypted = p->encrypted;
    row->running = running != NULL && p->type == ESP_PARTITION_TYPE_APP && p->address == running->address;
    row->address = p->address;
    row->size = p->size;

    it = esp_partition_next(it); /* NULL, and already released, past the last one */
  }
  esp_partition_iterator_release(it); /* a no-op for NULL */
}

/* Add @p name to the namespace list unless it is there already. False when it is new and the list is full. */
static bool _note_namespace(heaptop_storage_t *out, const char *name)
{
  for (uint8_t i = 0; i < out->namespace_count; i++)
  {
    if (strcmp(out->namespaces[i].name, name) == 0)
      return true;
  }
  if (out->namespace_count >= HEAPTOP_STORAGE_MAX_NAMESPACES)
    return false;

  heaptop_nvs_namespace_t *ns = &out->namespaces[out->namespace_count++];
  strlcpy(ns->name, name, sizeof(ns->name));
  ns->entries = 0;
  return true;
}

static void _read_nvs(const char *label, heaptop_storage_t *out)
{
  nvs_stats_t stats;
  if (nvs_get_stats(label, &stats) != ESP_OK)
    return; /* not initialised, or no such partition: nvs_present stays false */

  out->nvs_present = true;
  strlcpy(out->nvs_label, label != NULL ? label : NVS_DEFAULT_PART_NAME, sizeof(out->nvs_label));
  out->nvs_entry_bytes = HEAPTOP_NVS_ENTRY_BYTES;
  out->nvs_total = (uint32_t)stats.total_entries;
  out->nvs_used = (uint32_t)stats.used_entries;
  out->nvs_available = (uint32_t)stats.available_entries;

  /* The names first, with the iterator released, then the count of each: opening a namespace inside the walk would
   * take the NVS lock while the walk is still going. */
  nvs_iterator_t it = NULL;
  esp_err_t err = nvs_entry_find(label != NULL ? label : NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
  while (err == ESP_OK)
  {
    nvs_entry_info_t info;
    if (nvs_entry_info(it, &info) == ESP_OK && !_note_namespace(out, info.namespace_name))
    {
      out->namespaces_truncated = true;
      break;
    }
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);

  for (uint8_t i = 0; i < out->namespace_count; i++)
  {
    nvs_handle_t handle;
    if (nvs_open_from_partition(out->nvs_label, out->namespaces[i].name, NVS_READONLY, &handle) != ESP_OK)
      continue;
    size_t used = 0;
    if (nvs_get_used_entry_count(handle, &used) == ESP_OK)
      out->namespaces[i].entries = (uint32_t)used + 1; /* the namespace's own entry is not in the count */
    nvs_close(handle);
  }
}

esp_err_t heaptop_storage_read(const char *nvs_label, heaptop_storage_t *out)
{
  if (out == NULL)
    return ESP_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));

  uint32_t flash_size = 0;
  if (esp_flash_get_size(NULL, &flash_size) == ESP_OK)
    out->flash_size = flash_size;

  _read_partitions(out);
  _read_nvs(nvs_label, out);
  return ESP_OK;
}
