package zone.dice.dicenext

object Native {
    val available: Boolean = try {
        try {
            System.loadLibrary("c++_shared")
        } catch (_: UnsatisfiedLinkError) {
            // packaged with libdice-next.so; ignore if the linker already pulled it in
        }
        System.loadLibrary("dice-next")
        true
    } catch (_: UnsatisfiedLinkError) {
        false
    } catch (_: ExceptionInInitializerError) {
        false
    }

    /** 把 ConnectivityManager 交给 c-ares，否则 Drogon 在 Android 上解析不了公网域名。 */
    @JvmStatic external fun initDns(connectivityManager: Any)

    /** 阻塞直到核心退出。返回 42 表示面板请求重启。 */
    @JvmStatic external fun run(filesDir: String): Int

    @JvmStatic external fun stop()

    @JvmStatic external fun isRunning(): Boolean

    /** Native update service calls this on the worker thread to open the system installer. Empty string = ok. */
    @JvmStatic
    fun installApk(path: String): String = App.installUpdate(path)
}
