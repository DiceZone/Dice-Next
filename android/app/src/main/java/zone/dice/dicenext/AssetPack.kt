package zone.dice.dicenext

import android.content.Context
import java.io.File

/** 把 APK 里的面板和内置资源拷到应用私有目录，核心从文件系统读。 */
object AssetPack {
    private const val PREF = "dicenext"
    private const val KEY_PACK = "asset_version"

    fun sync(context: Context) {
        val files = context.filesDir
        files.mkdirs()
        File(files, "tmp").mkdirs()
        File(files, "bin").mkdirs()
        File(files, "data").mkdirs()
        File(files, "config").mkdirs()
        val version = try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionCode.toString()
        } catch (_: Exception) {
            "0"
        }
        val prefs = context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
        val packed = listOf(
            "web" to File(files, "web"),
            "i18n" to File(files, "i18n"),
            "decks" to File(files, "decks"),
            "card-templates" to File(files, "card-templates"),
            "docs" to File(files, "docs"),
            "data/helpdoc" to File(files, "data/helpdoc"),
            "data/plugins" to File(files, "data/plugins"),
        )
        val needRefresh = prefs.getString(KEY_PACK, "") != version ||
            !File(files, "web/dist/index.html").isFile
        if (needRefresh) {
            for ((asset, target) in packed) {
                if (hasAsset(context, asset)) {
                    target.deleteRecursively()
                    copyAssetDir(context, asset, target)
                }
            }
            copyAssetFile(context, "update-mirrors.json", File(files, "update-mirrors.json"))
            prefs.edit().putString(KEY_PACK, version).apply()
        }
        copyAssetFile(context, "cacert.pem", File(files, "cacert.pem"))
    }

    private fun hasAsset(context: Context, path: String): Boolean {
        return try {
            val names = context.assets.list(path)
            names != null && (names.isNotEmpty() || runCatching { context.assets.open(path).close() }.isSuccess)
        } catch (_: Exception) {
            false
        }
    }

    private fun copyAssetFile(context: Context, assetPath: String, target: File) {
        try {
            context.assets.open(assetPath).use { input ->
                target.parentFile?.mkdirs()
                target.outputStream().use { output -> input.copyTo(output) }
            }
        } catch (_: Exception) {
            // bundled file is optional
        }
    }

    private fun copyAssetDir(context: Context, assetPath: String, target: File) {
        val names = context.assets.list(assetPath) ?: return
        if (names.isEmpty()) {
            copyAssetFile(context, assetPath, target)
            return
        }
        target.mkdirs()
        for (name in names) copyAssetDir(context, "$assetPath/$name", File(target, name))
    }
}
