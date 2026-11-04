// Fingerprint / face unlock of NixSCP's encrypted configuration: the NixSCP
// password is encrypted with an AES key of the Android Keystore that only a
// strong biometric authentication releases (BiometricPrompt with a
// CryptoObject); a new fingerprint enrolled on the phone invalidates it.
// Called from C++ (src/mobile/AppLock.cpp); results come back through the
// native onResult.
package com.nixscp.android;

import android.app.Activity;
import android.content.Context;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.hardware.fingerprint.FingerprintManager;
import android.os.Build;
import android.os.CancellationSignal;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyPermanentlyInvalidatedException;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public class BiometricHelper
{
    private static final String ALIAS = "nixscp-unlock";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";

    // ok: data is the stored form (encrypt) or the secret (decrypt);
    // otherwise error ("invalidated", "cancelled" or Android's message)
    static native void onResult(int request, boolean ok, String data, String error);

    @SuppressWarnings("deprecation")
    public static boolean available(Context context)
    {
        if (Build.VERSION.SDK_INT >= 30) {
            BiometricManager manager = context.getSystemService(BiometricManager.class);
            return (manager != null) &&
                (manager.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG) == BiometricManager.BIOMETRIC_SUCCESS);
        }
        if (Build.VERSION.SDK_INT >= 29) {
            BiometricManager manager = context.getSystemService(BiometricManager.class);
            return (manager != null) && (manager.canAuthenticate() == BiometricManager.BIOMETRIC_SUCCESS);
        }
        FingerprintManager manager = context.getSystemService(FingerprintManager.class);
        return (manager != null) && manager.isHardwareDetected() && manager.hasEnrolledFingerprints();
    }

    private static SecretKey newKey() throws Exception
    {
        KeyGenParameterSpec.Builder builder = new KeyGenParameterSpec.Builder(ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setUserAuthenticationRequired(true)
            .setInvalidatedByBiometricEnrollment(true);
        if (Build.VERSION.SDK_INT >= 30)
            builder.setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG);
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(builder.build());
        return generator.generateKey();
    }

    private static SecretKey existingKey() throws Exception
    {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        return (SecretKey) store.getKey(ALIAS, null);
    }

    public static void forget()
    {
        try {
            KeyStore store = KeyStore.getInstance("AndroidKeyStore");
            store.load(null);
            store.deleteEntry(ALIAS);
        } catch (Exception e) {
            // nothing stored
        }
    }

    // Stores Secret: a new key, the fingerprint to use it; the result
    // ("<iv>:<ciphertext>", base64) is kept by the caller
    public static void encrypt(final Activity activity, final int request, final String secret)
    {
        activity.runOnUiThread(() -> {
            try {
                forget();
                Cipher cipher = Cipher.getInstance(TRANSFORMATION);
                cipher.init(Cipher.ENCRYPT_MODE, newKey());
                prompt(activity, request, cipher, "Turn on fingerprint unlock", (Cipher done) -> {
                    byte[] data = done.doFinal(secret.getBytes(StandardCharsets.UTF_8));
                    return Base64.encodeToString(done.getIV(), Base64.NO_WRAP) + ":" +
                        Base64.encodeToString(data, Base64.NO_WRAP);
                });
            } catch (Exception e) {
                onResult(request, false, null, String.valueOf(e.getMessage()));
            }
        });
    }

    // Gives the secret back after the fingerprint
    public static void decrypt(final Activity activity, final int request, final String stored)
    {
        activity.runOnUiThread(() -> {
            try {
                String[] parts = stored.split(":");
                byte[] iv = Base64.decode(parts[0], Base64.NO_WRAP);
                final byte[] data = Base64.decode(parts[1], Base64.NO_WRAP);
                SecretKey key = existingKey();
                if (key == null) {
                    onResult(request, false, null, "invalidated");
                    return;
                }
                Cipher cipher = Cipher.getInstance(TRANSFORMATION);
                cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
                prompt(activity, request, cipher, "Unlock NixSCP", (Cipher done) ->
                    new String(done.doFinal(data), StandardCharsets.UTF_8));
            } catch (KeyPermanentlyInvalidatedException e) {
                forget();
                onResult(request, false, null, "invalidated");
            } catch (Exception e) {
                onResult(request, false, null, String.valueOf(e.getMessage()));
            }
        });
    }

    private interface Finish
    {
        String run(Cipher cipher) throws Exception;
    }

    private static void prompt(Activity activity, final int request, Cipher cipher, String title, final Finish finish)
    {
        BiometricPrompt.Builder builder = new BiometricPrompt.Builder(activity)
            .setTitle(title)
            .setSubtitle("NixSCP Pro")
            .setNegativeButton("Use password", activity.getMainExecutor(),
                (dialog, which) -> onResult(request, false, null, "cancelled"));
        if (Build.VERSION.SDK_INT >= 30)
            builder.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG);
        builder.build().authenticate(new BiometricPrompt.CryptoObject(cipher), new CancellationSignal(),
            activity.getMainExecutor(), new BiometricPrompt.AuthenticationCallback() {
                @Override
                public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result)
                {
                    try {
                        onResult(request, true, finish.run(result.getCryptoObject().getCipher()), null);
                    } catch (Exception e) {
                        onResult(request, false, null, String.valueOf(e.getMessage()));
                    }
                }

                @Override
                public void onAuthenticationError(int code, CharSequence message)
                {
                    boolean cancelled = (code == BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED) ||
                        (code == BiometricPrompt.BIOMETRIC_ERROR_CANCELED);
                    onResult(request, false, null, cancelled ? "cancelled" : String.valueOf(message));
                }
            });
    }
}
