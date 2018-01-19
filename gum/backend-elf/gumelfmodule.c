/*
 * Copyright (C) 2010-2018 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumelfmodule.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum
{
  PROP_0,
  PROP_NAME,
  PROP_PATH,
  PROP_BASE_ADDRESS
};

typedef struct _GumElfEnumerateDepsContext GumElfEnumerateDepsContext;
typedef struct _GumElfEnumerateImportsContext GumElfEnumerateImportsContext;
typedef struct _GumElfEnumerateExportsContext GumElfEnumerateExportsContext;
typedef struct _GumElfStoreSymtabParamsContext GumElfStoreSymtabParamsContext;

struct _GumElfEnumerateDepsContext
{
  GumElfFoundDependencyFunc func;
  gpointer user_data;

  GumElfModule * module;
  const gchar * strtab;
};

struct _GumElfEnumerateImportsContext
{
  GumFoundImportFunc func;
  gpointer user_data;
};

struct _GumElfEnumerateExportsContext
{
  GumFoundExportFunc func;
  gpointer user_data;
};

struct _GumElfStoreSymtabParamsContext
{
  guint pending;

  gpointer entries;
  gsize entry_size;
  gsize entry_count;
  const gchar * strtab;

  GumElfModule * module;
};

static void gum_elf_module_constructed (GObject * object);
static void gum_elf_module_finalize (GObject * object);
static void gum_elf_module_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gum_elf_module_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);

static gboolean gum_store_strtab (const GumElfDynamicEntryDetails * details,
    gpointer user_data);
static gboolean gum_emit_each_needed (const GumElfDynamicEntryDetails * details,
    gpointer user_data);
static gboolean gum_emit_elf_import (const GumElfSymbolDetails * details,
    gpointer user_data);
static gboolean gum_emit_elf_export (const GumElfSymbolDetails * details,
    gpointer user_data);
static gboolean gum_store_symtab_params (
    const GumElfDynamicEntryDetails * details, gpointer user_data);
static void gum_elf_module_enumerate_symbols_in_section (GumElfModule * self,
    GumElfSectionHeaderType section, GumElfFoundSymbolFunc func,
    gpointer user_data);
static gboolean gum_elf_module_find_dynamic_range (GumElfModule * self,
    GumMemoryRange * range);
static GumAddress gum_elf_module_compute_preferred_address (
    GumElfModule * self);
static GumAddress gum_elf_module_resolve_virtual_address (GumElfModule * self,
    GumAddress address);

G_DEFINE_TYPE (GumElfModule, gum_elf_module, G_TYPE_OBJECT)

static void
gum_elf_module_class_init (GumElfModuleClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = gum_elf_module_constructed;
  object_class->finalize = gum_elf_module_finalize;
  object_class->get_property = gum_elf_module_get_property;
  object_class->set_property = gum_elf_module_set_property;

  g_object_class_install_property (object_class, PROP_NAME,
      g_param_spec_string ("name", "Name", "Name", NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PATH,
      g_param_spec_string ("path", "Path", "Path", NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_BASE_ADDRESS,
      g_param_spec_uint64 ("base-address", "BaseAddress", "Base address", 0,
      G_MAXUINT64, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gum_elf_module_init (GumElfModule * self)
{
}

static void
gum_elf_module_constructed (GObject * object)
{
  GumElfModule * self = GUM_ELF_MODULE (object);
  int fd;
  GElf_Half type;

  if (self->name == NULL)
  {
    self->name = g_path_get_basename (self->path);
  }

  fd = open (self->path, O_RDONLY);
  if (fd == -1)
    goto error;

  self->file_size = lseek (fd, 0, SEEK_END);
  lseek (fd, 0, SEEK_SET);

  self->file_data = mmap (NULL, self->file_size, PROT_READ, MAP_PRIVATE, fd, 0);

  close (fd);

  if (self->file_data == MAP_FAILED)
    goto mmap_failed;

  self->elf = elf_memory (self->file_data, self->file_size);
  if (self->elf == NULL)
    goto error;

  self->ehdr = gelf_getehdr (self->elf, &self->ehdr_storage);

  type = self->ehdr->e_type;
  if (type != ET_EXEC && type != ET_DYN)
    goto error;

  self->preferred_address = gum_elf_module_compute_preferred_address (self);

  self->valid = TRUE;
  return;

mmap_failed:
  {
    self->file_data = NULL;
    goto error;
  }
error:
  {
    self->valid = FALSE;
    return;
  }
}

static void
gum_elf_module_finalize (GObject * object)
{
  GumElfModule * self = GUM_ELF_MODULE (object);

  if (self->elf != NULL)
    elf_end (self->elf);

  if (self->file_data != NULL)
    munmap (self->file_data, self->file_size);

  g_free (self->path);
  g_free (self->name);

  G_OBJECT_CLASS (gum_elf_module_parent_class)->finalize (object);
}

static void
gum_elf_module_get_property (GObject * object,
                             guint property_id,
                             GValue * value,
                             GParamSpec * pspec)
{
  GumElfModule * self = GUM_ELF_MODULE (object);

  switch (property_id)
  {
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    case PROP_PATH:
      g_value_set_string (value, self->path);
      break;
    case PROP_BASE_ADDRESS:
      g_value_set_uint64 (value, self->base_address);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
gum_elf_module_set_property (GObject * object,
                             guint property_id,
                             const GValue * value,
                             GParamSpec * pspec)
{
  GumElfModule * self = GUM_ELF_MODULE (object);

  switch (property_id)
  {
    case PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;
    case PROP_PATH:
      g_free (self->path);
      self->path = g_value_dup_string (value);
      break;
    case PROP_BASE_ADDRESS:
      self->base_address = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

GumElfModule *
gum_elf_module_new_from_memory (const gchar * path,
                                GumAddress base_address)
{
  GumElfModule * module;

  module = g_object_new (GUM_ELF_TYPE_MODULE,
      "path", path,
      "base-address", base_address,
      NULL);
  if (!module->valid)
  {
    g_object_unref (module);
    return NULL;
  }

  return module;
}

void
gum_elf_module_enumerate_dependencies (GumElfModule * self,
                                       GumElfFoundDependencyFunc func,
                                       gpointer user_data)
{
  GumElfEnumerateDepsContext ctx;

  ctx.func = func;
  ctx.user_data = user_data;

  ctx.module = self;
  ctx.strtab = NULL;

  gum_elf_module_enumerate_dynamic_entries (self, gum_store_strtab, &ctx);
  if (ctx.strtab == NULL)
    return;

  gum_elf_module_enumerate_dynamic_entries (self, gum_emit_each_needed, &ctx);
}

static gboolean
gum_store_strtab (const GumElfDynamicEntryDetails * details,
                  gpointer user_data)
{
  GumElfEnumerateDepsContext * ctx = user_data;

  if (details->type != DT_STRTAB)
    return TRUE;

  ctx->strtab = GSIZE_TO_POINTER (
      gum_elf_module_resolve_virtual_address (ctx->module, details->value));
  return FALSE;
}

static gboolean
gum_emit_each_needed (const GumElfDynamicEntryDetails * details,
                      gpointer user_data)
{
  GumElfEnumerateDepsContext * ctx = user_data;
  GumElfDependencyDetails d;

  if (details->type != DT_NEEDED)
    return TRUE;

  d.name = ctx->strtab + details->value;

  return ctx->func (&d, ctx->user_data);
}

void
gum_elf_module_enumerate_imports (GumElfModule * self,
                                  GumFoundImportFunc func,
                                  gpointer user_data)
{
  GumElfEnumerateImportsContext ctx;

  ctx.func = func;
  ctx.user_data = user_data;

  gum_elf_module_enumerate_dynamic_symbols (self, gum_emit_elf_import, &ctx);
}

static gboolean
gum_emit_elf_import (const GumElfSymbolDetails * details,
                     gpointer user_data)
{
  GumElfEnumerateImportsContext * ctx = user_data;

  if (details->section_header_index == SHN_UNDEF &&
      (details->type == STT_FUNC || details->type == STT_OBJECT))
  {
    GumImportDetails d;

    d.type = (details->type == STT_FUNC)
        ? GUM_EXPORT_FUNCTION
        : GUM_EXPORT_VARIABLE;
    d.name = details->name;
    d.module = NULL;
    d.address = 0;

    if (!ctx->func (&d, ctx->user_data))
      return FALSE;
  }

  return TRUE;
}

void
gum_elf_module_enumerate_exports (GumElfModule * self,
                                  GumFoundExportFunc func,
                                  gpointer user_data)
{
  GumElfEnumerateExportsContext ctx;

  ctx.func = func;
  ctx.user_data = user_data;

  gum_elf_module_enumerate_dynamic_symbols (self, gum_emit_elf_export, &ctx);
}

static gboolean
gum_emit_elf_export (const GumElfSymbolDetails * details,
                     gpointer user_data)
{
  GumElfEnumerateExportsContext * ctx = user_data;

  if (details->section_header_index != SHN_UNDEF &&
      (details->type == STT_FUNC || details->type == STT_OBJECT) &&
      (details->bind == STB_GLOBAL || details->bind == STB_WEAK))
  {
    GumExportDetails d;

    d.type = (details->type == STT_FUNC)
        ? GUM_EXPORT_FUNCTION
        : GUM_EXPORT_VARIABLE;
    d.name = details->name;
    d.address = details->address;

    if (!ctx->func (&d, ctx->user_data))
      return FALSE;
  }

  return TRUE;
}

void
gum_elf_module_enumerate_dynamic_symbols (GumElfModule * self,
                                          GumElfFoundSymbolFunc func,
                                          gpointer user_data)
{
  GumElfStoreSymtabParamsContext ctx;
  gsize entry_index;

  ctx.pending = 4;

  ctx.entries = NULL;
  ctx.entry_size = 0;
  ctx.entry_count = 0;
  ctx.strtab = NULL;

  ctx.module = self;

  gum_elf_module_enumerate_dynamic_entries (self, gum_store_symtab_params,
      &ctx);
  if (ctx.pending != 0)
    return;

  for (entry_index = 1; entry_index < ctx.entry_count; entry_index++)
  {
    gpointer entry = ctx.entries + (entry_index * ctx.entry_size);
    GumElfSymbolDetails details;

    if (sizeof (gpointer) == 4)
    {
      Elf32_Sym * sym = entry;

      details.name = ctx.strtab + sym->st_name;
      details.address =
          gum_elf_module_resolve_virtual_address (self, sym->st_value);
      details.type = GELF_ST_TYPE (sym->st_info);
      details.bind = GELF_ST_BIND (sym->st_info);
      details.section_header_index = sym->st_shndx;
    }
    else
    {
      Elf64_Sym * sym = entry;

      details.name = ctx.strtab + sym->st_name;
      details.address =
          gum_elf_module_resolve_virtual_address (self, sym->st_value);
      details.type = GELF_ST_TYPE (sym->st_info);
      details.bind = GELF_ST_BIND (sym->st_info);
      details.section_header_index = sym->st_shndx;
    }

    if (!func (&details, user_data))
      return;
  }
}

static gboolean
gum_store_symtab_params (const GumElfDynamicEntryDetails * details,
                         gpointer user_data)
{
  GumElfStoreSymtabParamsContext * ctx = user_data;

  switch (details->type)
  {
    case DT_SYMTAB:
      ctx->entries = GSIZE_TO_POINTER (
          gum_elf_module_resolve_virtual_address (ctx->module, details->value));
      ctx->pending--;
      break;
    case DT_SYMENT:
      ctx->entry_size = details->value;
      ctx->pending--;
      break;
    case DT_HASH:
    {
      guint32 * hash_params, nchain;

      hash_params = GSIZE_TO_POINTER (
          gum_elf_module_resolve_virtual_address (ctx->module, details->value));
      nchain = hash_params[1];

      ctx->entry_count = nchain;
      ctx->pending--;

      break;
    }
    case DT_STRTAB:
      ctx->strtab = GSIZE_TO_POINTER (
          gum_elf_module_resolve_virtual_address (ctx->module, details->value));
      ctx->pending--;
      break;
    default:
      break;
  }

  return ctx->pending != 0;
}

void
gum_elf_module_enumerate_symbols (GumElfModule * self,
                                  GumElfFoundSymbolFunc func,
                                  gpointer user_data)
{
  gum_elf_module_enumerate_symbols_in_section (self, SHT_SYMTAB, func,
      user_data);
}

static void
gum_elf_module_enumerate_symbols_in_section (GumElfModule * self,
                                             GumElfSectionHeaderType section,
                                             GumElfFoundSymbolFunc func,
                                             gpointer user_data)
{
  Elf_Scn * scn;
  GElf_Shdr shdr;
  gboolean carry_on;
  GElf_Word symbol_count, symbol_index;
  Elf_Data * data;

  if (!gum_elf_module_find_section_header (self, section, &scn, &shdr))
    return;

  carry_on = TRUE;
  symbol_count = shdr.sh_size / shdr.sh_entsize;
  data = elf_getdata (scn, NULL);

  for (symbol_index = 0;
      symbol_index != symbol_count && carry_on;
      symbol_index++)
  {
    GElf_Sym sym;
    GumElfSymbolDetails details;

    gelf_getsym (data, symbol_index, &sym);

    details.name = elf_strptr (self->elf, shdr.sh_link, sym.st_name);
    details.address =
        gum_elf_module_resolve_virtual_address (self, sym.st_value);
    details.type = GELF_ST_TYPE (sym.st_info);
    details.bind = GELF_ST_BIND (sym.st_info);
    details.section_header_index = sym.st_shndx;

    carry_on = func (&details, user_data);
  }
}

void
gum_elf_module_enumerate_dynamic_entries (GumElfModule * self,
                                          GumElfFoundDynamicEntryFunc func,
                                          gpointer user_data)
{
  GumMemoryRange dynamic;
  gpointer dynamic_begin;

  if (!gum_elf_module_find_dynamic_range (self, &dynamic))
    return;

  dynamic_begin = GSIZE_TO_POINTER (
      gum_elf_module_resolve_virtual_address (self, dynamic.base_address));

  if (sizeof (gpointer) == 4)
  {
    Elf32_Dyn * entries;
    guint entry_count, entry_index;

    entries = dynamic_begin;
    entry_count = dynamic.size / sizeof (Elf32_Dyn);

    for (entry_index = 0; entry_index != entry_count; entry_index++)
    {
      Elf32_Dyn * entry = &entries[entry_index];
      GumElfDynamicEntryDetails d;

      d.type = entry->d_tag;
      d.value = entry->d_un.d_val;

      if (!func (&d, user_data))
        return;
    }
  }
  else
  {
    Elf64_Dyn * entries;
    guint entry_count, entry_index;

    entries = dynamic_begin;
    entry_count = dynamic.size / sizeof (Elf64_Dyn);

    for (entry_index = 0; entry_index != entry_count; entry_index++)
    {
      Elf64_Dyn * entry = &entries[entry_index];
      GumElfDynamicEntryDetails d;

      d.type = entry->d_tag;
      d.value = entry->d_un.d_val;

      if (!func (&d, user_data))
        return;
    }
  }
}

static gboolean
gum_elf_module_find_dynamic_range (GumElfModule * self,
                                   GumMemoryRange * range)
{
  GElf_Half header_count, header_index;

  header_count = self->ehdr->e_phnum;
  for (header_index = 0; header_index != header_count; header_index++)
  {
    GElf_Phdr phdr;

    gelf_getphdr (self->elf, header_index, &phdr);

    if (phdr.p_type == PT_DYNAMIC)
    {
      range->base_address = phdr.p_vaddr;
      range->size = phdr.p_memsz;
      return TRUE;
    }
  }

  return FALSE;
}

gboolean
gum_elf_module_find_section_header (GumElfModule * self,
                                    GumElfSectionHeaderType type,
                                    Elf_Scn ** scn,
                                    GElf_Shdr * shdr)
{
  Elf_Scn * cur = NULL;

  while ((cur = elf_nextscn (self->elf, cur)) != NULL)
  {
    gelf_getshdr (cur, shdr);

    if (shdr->sh_type == type)
    {
      *scn = cur;
      return TRUE;
    }
  }

  return FALSE;
}

static GumAddress
gum_elf_module_compute_preferred_address (GumElfModule * self)
{
  GElf_Half header_count, header_index;

  header_count = self->ehdr->e_phnum;

  for (header_index = 0; header_index != header_count; header_index++)
  {
    GElf_Phdr phdr;

    gelf_getphdr (self->elf, header_index, &phdr);

    if (phdr.p_offset == 0)
      return phdr.p_vaddr;
  }

  return 0;
}

static GumAddress
gum_elf_module_resolve_virtual_address (GumElfModule * self,
                                        GumAddress address)
{
  return self->base_address + (address - self->preferred_address);
}
