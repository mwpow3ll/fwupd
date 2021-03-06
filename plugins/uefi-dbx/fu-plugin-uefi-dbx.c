/*
 * Copyright (C) 2020 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fu-plugin-vfuncs.h"
#include "fu-efivar.h"
#include "fu-hash.h"
#include "fu-uefi-dbx-common.h"
#include "fu-uefi-dbx-file.h"

struct FuPluginData {
	gchar			*fn;
};

void
fu_plugin_init (FuPlugin *plugin)
{
	fu_plugin_alloc_data (plugin, sizeof (FuPluginData));
	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
}

void
fu_plugin_destroy (FuPlugin *plugin)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_free (data->fn);
}

gboolean
fu_plugin_startup (FuPlugin *plugin, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	data->fn = fu_uefi_dbx_get_dbxupdate (error);
	if (data->fn == NULL)
		return FALSE;
	g_debug ("using %s", data->fn);
	return TRUE;
}

void
fu_plugin_add_security_attrs (FuPlugin *plugin, FuSecurityAttrs *attrs)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	GPtrArray *checksums;
	gsize bufsz = 0;
	guint missing_cnt = 0;
	g_autofree guint8 *buf_system = NULL;
	g_autofree guint8 *buf_update = NULL;
	g_autoptr(FuUefiDbxFile) dbx_system = NULL;
	g_autoptr(FuUefiDbxFile) dbx_update = NULL;
	g_autoptr(FwupdSecurityAttr) attr = NULL;
	g_autoptr(GError) error_local = NULL;

	/* create attr */
	attr = fwupd_security_attr_new ("org.uefi.SecureBoot.dbx");
	fwupd_security_attr_set_level (attr, FWUPD_SECURITY_ATTR_LEVEL_CRITICAL);
	fwupd_security_attr_set_name (attr, "UEFI dbx");
	fu_security_attrs_append (attrs, attr);

	/* no binary blob */
	if (!fu_plugin_get_enabled (plugin)) {
		g_autofree gchar *dbxdir = NULL;
		g_autofree gchar *result = NULL;
		dbxdir = fu_common_get_path (FU_PATH_KIND_EFIDBXDIR);
		result = g_strdup_printf ("DBX can be downloaded from %s and decompressed into %s: ",
					  FU_UEFI_DBX_DATA_URL, dbxdir);
		fwupd_security_attr_set_result (attr, result);
		return;
	}

	/* get update dbx */
	if (!g_file_get_contents (data->fn, (gchar **) &buf_update, &bufsz, &error_local)) {
		g_warning ("failed to load %s: %s", data->fn, error_local->message);
		fwupd_security_attr_set_result (attr, "Failed to load update DBX");
		return;
	}
	dbx_update = fu_uefi_dbx_file_new (buf_update, bufsz,
					   FU_UEFI_DBX_FILE_PARSE_FLAGS_IGNORE_HEADER,
					   &error_local);
	if (dbx_update == NULL) {
		g_warning ("failed to parse %s: %s", data->fn, error_local->message);
		fwupd_security_attr_set_result (attr, "Failed to parse update DBX");
		return;
	}

	/* get system dbx */
	if (!fu_efivar_get_data ("d719b2cb-3d3a-4596-a3bc-dad00e67656f", "dbx",
				 &buf_system, &bufsz, NULL, &error_local)) {
		g_warning ("failed to load EFI dbx: %s", error_local->message);
		fwupd_security_attr_set_result (attr, "Failed to load EFI DBX");
		return;
	}
	dbx_system = fu_uefi_dbx_file_new (buf_system, bufsz,
					   FU_UEFI_DBX_FILE_PARSE_FLAGS_NONE,
					   &error_local);
	if (dbx_system == NULL) {
		g_warning ("failed to parse EFI dbx: %s", error_local->message);
		fwupd_security_attr_set_result (attr, "Failed to parse EFI DBX");
		return;
	}

	/* look for each checksum in the update in the system version */
	checksums = fu_uefi_dbx_file_get_checksums (dbx_update);
	for (guint i = 0; i < checksums->len; i++) {
		const gchar *checksum = g_ptr_array_index (checksums, i);
		if (!fu_uefi_dbx_file_has_checksum (dbx_system, checksum)) {
			g_debug ("%s missing from the system DBX", checksum);
			missing_cnt += 1;
		}
	}

	/* add security attribute */
	if (missing_cnt > 0) {
		g_autofree gchar *summary = g_strdup_printf ("%u hashes missing", missing_cnt);
		fwupd_security_attr_set_result (attr, summary);
		return;
	}

	/* success */
	fwupd_security_attr_add_flag (attr, FWUPD_SECURITY_ATTR_FLAG_SUCCESS);
}
