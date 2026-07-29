package com.inputflow.android

import android.annotation.SuppressLint
import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Stores the Android relay pairing secret with an app-owned Android Keystore
 * key. Existing plaintext preferences are migrated once and then removed.
 */
object PairingSecretStore {
    private const val TAG = "PairingSecretStore"
    private const val KEY_ALIAS = "inputflow_pairing_secret_v1"
    private const val LEGACY_SECRET_KEY = "secret"
    private const val CIPHERTEXT_KEY = "pairing_secret_ciphertext_v1"
    private const val IV_KEY = "pairing_secret_iv_v1"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"

    @Synchronized
    fun read(context: Context): String {
        val prefs = context.getSharedPreferences(
            RelayForegroundService.PREFS,
            Context.MODE_PRIVATE,
        )
        val ciphertext = prefs.getString(CIPHERTEXT_KEY, null)
        val iv = prefs.getString(IV_KEY, null)
        if (!ciphertext.isNullOrBlank() && !iv.isNullOrBlank()) {
            return try {
                val cipher = Cipher.getInstance(TRANSFORMATION)
                cipher.init(
                    Cipher.DECRYPT_MODE,
                    encryptionKey(),
                    GCMParameterSpec(128, Base64.decode(iv, Base64.NO_WRAP)),
                )
                cipher.updateAAD(context.packageName.toByteArray(Charsets.UTF_8))
                String(
                    cipher.doFinal(Base64.decode(ciphertext, Base64.NO_WRAP)),
                    Charsets.UTF_8,
                )
            } catch (_: Exception) {
                Log.e(TAG, "Stored pairing secret could not be decrypted")
                ""
            }
        }

        val legacy = prefs.getString(LEGACY_SECRET_KEY, "").orEmpty()
        if (legacy.isBlank()) return ""
        if (!save(context, legacy)) {
            Log.e(TAG, "Plaintext pairing secret migration failed")
            return ""
        }
        return legacy
    }

    @Synchronized
    @SuppressLint("ApplySharedPref")
    fun save(context: Context, secret: String): Boolean {
        val prefs = context.getSharedPreferences(
            RelayForegroundService.PREFS,
            Context.MODE_PRIVATE,
        )
        if (secret.isBlank()) {
            return prefs.edit()
                .remove(CIPHERTEXT_KEY)
                .remove(IV_KEY)
                .remove(LEGACY_SECRET_KEY)
                .commit()
        }

        return try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, encryptionKey())
            cipher.updateAAD(context.packageName.toByteArray(Charsets.UTF_8))
            val ciphertext = cipher.doFinal(secret.toByteArray(Charsets.UTF_8))
            prefs.edit()
                .putString(CIPHERTEXT_KEY, Base64.encodeToString(ciphertext, Base64.NO_WRAP))
                .putString(IV_KEY, Base64.encodeToString(cipher.iv, Base64.NO_WRAP))
                .remove(LEGACY_SECRET_KEY)
                .commit()
        } catch (_: Exception) {
            Log.e(TAG, "Pairing secret could not be protected")
            false
        }
    }

    private fun encryptionKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }

        val generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES,
            "AndroidKeyStore",
        )
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }
}
