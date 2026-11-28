package zone.dice.catofficial

import android.content.Context
import java.io.File

/** 把 APK 里的 web/ 面板拷到应用私有目录，核心从文件系统读。 */
object AssetPack {
    private const val PREF = "catofficial"
    private const val KEY_WEB = "web_version"

    fun sync(context: Context) {
        val files = context.filesDir
        files.mkdirs()
        File(files, "tmp").mkdirs()
        File(files, "bin").mkdirs()
        File(files, "data").mkdirs()
        val version = try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionCode.toString()
        } catch (_: Exception) {
            "0"
        }
        val prefs = context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
        val webDir = File(files, "web")
        if (prefs.getString(KEY_WEB, "") != version || !File(webDir, "index.html").isFile) {
            webDir.deleteRecursively()
            copyAssetDir(context, "web", webDir)
            prefs.edit().putString(KEY_WEB, version).apply()
        }
        val cert = File(files, "cacert.pem")
        try {
            context.assets.open("cacert.pem").use { input ->
                cert.outputStream().use { output -> input.copyTo(output) }
            }
        } catch (_: Exception) {
            // 没有随包装 CA 时走系统证书目录
        }
    }

    private fun copyAssetDir(context: Context, assetPath: String, target: File) {
        val names = context.assets.list(assetPath) ?: return
        if (names.isEmpty()) {
            target.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        target.mkdirs()
        for (name in names) copyAssetDir(context, "$assetPath/$name", File(target, name))
    }
}
