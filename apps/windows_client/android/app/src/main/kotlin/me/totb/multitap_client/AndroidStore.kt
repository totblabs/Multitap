package me.totb.multitap_client

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** App-private preferences; only explicitly remembered passwords enter AES-GCM storage. */
class AndroidStore(context: Context, engine: FlutterEngine) {
    private val prefs = context.getSharedPreferences("multitap", Context.MODE_PRIVATE)
    private val alias = "me.multitap.login.v1"
    init {
        MethodChannel(engine.dartExecutor.binaryMessenger, "multitap/android/store").setMethodCallHandler { call, result ->
            try {
                val origin = call.argument<String>("origin") ?: ""
                when (call.method) {
                    "origin" -> {
                        val saved = prefs.getString("origin", null)
                        result.success(saved?.takeIf { AndroidTransportPolicy.credentialOriginAllowed(it, prefs.all) })
                    }
                    "transportSettings" -> result.success(AndroidTransportPolicy.settings(prefs.all))
                    "saveOrigin" -> {
                        val saved = requireNotNull(AndroidTransportPolicy.saveRequest(call.arguments))
                        val edit = prefs.edit().putInt("transportVersion", 2).putString("origin", saved["serverOrigin"] as String)
                            .remove("httpConsentOrigin")
                        (saved["httpConsentOrigin"] as? String)?.let { edit.putString("httpConsentOrigin", it) }
                        check(edit.commit())
                        result.success(null)
                    }
                    "login" -> {
                        require(AndroidTransportPolicy.credentialOriginAllowed(origin, prefs.all))
                        val id = prefs.getString("id:$origin", "") ?: ""
                        // Over HTTP a stored password is never read back.
                        val sealed = prefs.getString("password:$origin", null)
                            ?.takeIf { AndroidTransportPolicy.passwordMemoryAllowed(origin) }
                        val password = if (sealed == null) null else try { decrypt(sealed, origin, id) } catch (_: Exception) {
                            check(prefs.edit().remove("password:$origin").commit()); null
                        }
                        result.success(mapOf("username" to id, "password" to password))
                    }
                    "forgetPassword" -> {
                        require(AndroidTransportPolicy.normalize(origin) == origin)
                        check(prefs.edit().remove("password:$origin").commit()); result.success(null)
                    }
                    "saveLogin" -> {
                        require(AndroidTransportPolicy.credentialOriginAllowed(origin, prefs.all))
                        val id = call.argument<String>("username") ?: ""
                        require(AndroidPreferencePolicy.accountLengthAllowed(id))
                        val password = call.argument<String>("password")
                        // Remove the old account's secret before encryption/key access can fail.
                        check(prefs.edit().putString("id:$origin", id).remove("password:$origin").commit())
                        // Over HTTP nothing is remembered (the old one went above).
                        if (call.argument<Boolean>("remember") == true && password != null &&
                            AndroidTransportPolicy.passwordMemoryAllowed(origin)) {
                            require(password.length <= 256)
                            val encrypted = encrypt(password, origin, id)
                            check(prefs.edit().putString("password:$origin", encrypted).commit())
                        }
                        result.success(null)
                    }
                    // UI choices only; credential entries never enter this response.
                    "preferences" -> result.success(AndroidPreferencePolicy.read(prefs.all))
                    "savePreference" -> {
                        val key = call.argument<String>("key") ?: ""
                        val value = call.argument<String>("value") ?: ""
                        require(AndroidPreferencePolicy.allowsValue(key, value, prefs.all))
                        check(prefs.edit().putString("pref:$key", value).commit())
                        result.success(null)
                    }
                    // The running APK's own version, never a hand-maintained label.
                    "appVersion" -> result.success(packageVersion(context))
                    else -> result.notImplemented()
                }
            } catch (_: Exception) { result.error("storage_failed", MultitapText.get(context, R.string.storage_save_failed), null) }
        }
    }
    private fun packageVersion(context: Context): String? = try {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName
    } catch (_: Exception) { null }
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    private fun aad(origin: String, id: String) = "$origin\u0000$id".toByteArray(Charsets.UTF_8)
    private fun encrypt(value: String, origin: String, id: String): String {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key()); cipher.updateAAD(aad(origin, id))
        return Base64.encodeToString(cipher.iv + cipher.doFinal(value.toByteArray(Charsets.UTF_8)), Base64.NO_WRAP)
    }
    private fun decrypt(value: String, origin: String, id: String): String {
        val bytes = Base64.decode(value, Base64.NO_WRAP)
        require(bytes.size in 28..2048)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes.copyOfRange(0, 12)))
        cipher.updateAAD(aad(origin, id))
        return cipher.doFinal(bytes.copyOfRange(12, bytes.size)).toString(Charsets.UTF_8)
    }
}
