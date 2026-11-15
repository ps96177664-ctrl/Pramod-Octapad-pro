package com.pramod.piano;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.util.Log;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

final class OfflineAccessStore {
    private static final String TAG = "OfflineAccess";
    private static final String KEY_ALIAS = "piano-offline-access-v1";
    private static final String PREFS = "piano_offline_access";
    private static final String GRANT = "encrypted_authorized_uid";

    private final SharedPreferences preferences;

    OfflineAccessStore(Context context) {
        preferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    boolean saveAuthorizedUid(String uid) {
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey());
            byte[] encrypted = cipher.doFinal(uid.getBytes(StandardCharsets.UTF_8));
            byte[] iv = cipher.getIV();
            byte[] payload = new byte[iv.length + encrypted.length];
            System.arraycopy(iv, 0, payload, 0, iv.length);
            System.arraycopy(encrypted, 0, payload, iv.length, encrypted.length);
            return preferences.edit().putString(GRANT,
                    Base64.encodeToString(payload, Base64.NO_WRAP)).commit();
        } catch (GeneralSecurityException | IOException error) {
            Log.e(TAG, "Could not securely save the offline access grant", error);
            return false;
        }
    }

    boolean isAuthorized(String uid) {
        String encoded = preferences.getString(GRANT, null);
        if (encoded == null) return false;
        try {
            byte[] payload = Base64.decode(encoded, Base64.NO_WRAP);
            int ivLength = 12;
            if (payload.length <= ivLength) return false;
            byte[] iv = new byte[ivLength];
            byte[] encrypted = new byte[payload.length - ivLength];
            System.arraycopy(payload, 0, iv, 0, iv.length);
            System.arraycopy(payload, ivLength, encrypted, 0, encrypted.length);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), new GCMParameterSpec(128, iv));
            String authorizedUid = new String(cipher.doFinal(encrypted), StandardCharsets.UTF_8);
            return authorizedUid.equals(uid);
        } catch (IllegalArgumentException | GeneralSecurityException | IOException error) {
            Log.e(TAG, "Could not verify the encrypted offline access grant", error);
            return false;
        }
    }

    void clear() {
        if (!preferences.edit().remove(GRANT).commit()) {
            Log.e(TAG, "Could not remove the cached offline access grant");
        }
        try {
            KeyStore keyStore = KeyStore.getInstance("AndroidKeyStore");
            keyStore.load(null);
            if (keyStore.containsAlias(KEY_ALIAS)) keyStore.deleteEntry(KEY_ALIAS);
        } catch (GeneralSecurityException | IOException error) {
            Log.e(TAG, "Could not clear the offline access encryption key", error);
        }
    }

    private SecretKey getOrCreateKey() throws GeneralSecurityException, IOException {
        KeyStore keyStore = KeyStore.getInstance("AndroidKeyStore");
        keyStore.load(null);
        if (keyStore.containsAlias(KEY_ALIAS)) {
            KeyStore.Entry entry = keyStore.getEntry(KEY_ALIAS, null);
            if (entry instanceof KeyStore.SecretKeyEntry) {
                return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
            }
            keyStore.deleteEntry(KEY_ALIAS);
        }
        KeyGenerator generator = KeyGenerator.getInstance(
                KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build());
        return generator.generateKey();
    }
}
