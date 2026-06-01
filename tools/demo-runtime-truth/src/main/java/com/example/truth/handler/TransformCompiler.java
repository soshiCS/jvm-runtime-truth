package com.example.truth.handler;

import net.bytebuddy.ByteBuddy;
import net.bytebuddy.NamingStrategy;
import net.bytebuddy.dynamic.loading.ClassLoadingStrategy;
import net.bytebuddy.implementation.FixedValue;
import net.bytebuddy.matcher.ElementMatchers;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;
import java.util.concurrent.ConcurrentHashMap;
import java.util.zip.CRC32;

/**
 * Compiles a TransformHandler for a given (sessionKey, ciphertext) pair by:
 *   1. Computing a composite selection input:
 *      sessionKey + ":" + CRC32(ciphertext) + ":" + INSTANCE_TOKEN
 *   2. Hashing the composite input via a polynomial rolling hash to get the index.
 *   3. Looking up the XOR key for that index in transform-keys.properties.
 *   4. Generating a concrete subclass of TransformBase with the key baked in
 *      as a constant using ByteBuddy.
 *
 * INSTANCE_TOKEN is captured once at class load time from System.nanoTime().
 * It is never logged and cannot be determined through static source inspection.
 * Determining which transform index was selected for any given request therefore
 * requires runtime observation — the class name in causality output encodes it.
 *
 * The generated class name includes the transform index (e.g. TransformBase$Transform037$...)
 * so that monitoring tools can correlate a running handler back to its config entry.
 */
@Component
public class TransformCompiler {

    private static final String KEYS_CONFIG     = "transform-config/transform-keys.properties";
    private static final String SELECTOR_CONFIG = "transform-config/selector-config.properties";
    private static final int    HASH_MULTIPLIER  = 31;
    private static final int    TRANSFORM_COUNT  = 50;

    // Captured once at class-load time. Never logged, not in any config file.
    // Varies per JVM instance — static inspection cannot determine this value.
    // For controlled demo replay, set -Ddemo.instance.token=<long> to pin the value.
    private static final long   INSTANCE_TOKEN   =
        System.getProperties().containsKey("demo.instance.token")
            ? Long.parseLong(System.getProperty("demo.instance.token"))
            : System.nanoTime();

    private final int        hashSeed;
    private final Properties transformKeys;
    private final ConcurrentHashMap<String, TransformHandler> cache = new ConcurrentHashMap<>();

    public TransformCompiler() throws IOException {
        Properties selector = new Properties();
        try (InputStream in = TransformCompiler.class.getClassLoader()
                .getResourceAsStream(SELECTOR_CONFIG)) {
            if (in == null) throw new IOException(SELECTOR_CONFIG + " not found");
            selector.load(in);
        }
        this.hashSeed = Integer.parseInt(selector.getProperty("transform.hash.seed"));

        this.transformKeys = new Properties();
        try (InputStream in = TransformCompiler.class.getClassLoader()
                .getResourceAsStream(KEYS_CONFIG)) {
            if (in == null) throw new IOException(KEYS_CONFIG + " not found");
            transformKeys.load(in);
        }
    }

    public TransformHandler compile(String sessionKey, byte[] ciphertext) throws Exception {
        CRC32 crc = new CRC32();
        crc.update(ciphertext);
        String cacheKey = sessionKey + ":" + crc.getValue();
        return cache.computeIfAbsent(cacheKey, k -> {
            try {
                return doCompile(sessionKey, ciphertext);
            } catch (Exception e) {
                throw new RuntimeException("Compilation failed for sessionKey=" + sessionKey, e);
            }
        });
    }

    private TransformHandler doCompile(String sessionKey, byte[] ciphertext) throws Exception {
        int index = selectTransform(sessionKey, ciphertext);
        String keyProp = String.format("transform.%03d", index);
        int keyValue = Integer.parseInt(transformKeys.getProperty(keyProp));

        String transformTag = String.format("Transform%03d", index);

        Class<?> generated = new ByteBuddy()
                .with(new NamingStrategy.SuffixingRandom(transformTag))
                .subclass(TransformBase.class)
                .method(ElementMatchers.named("getKey"))
                .intercept(FixedValue.value(keyValue))
                .make()
                .load(TransformCompiler.class.getClassLoader(),
                      ClassLoadingStrategy.Default.INJECTION)
                .getLoaded();

        return (TransformHandler) generated.getDeclaredConstructor().newInstance();
    }

    /**
     * Maps a (sessionKey, ciphertext) pair to a transform index in [1, TRANSFORM_COUNT].
     *
     * The composite input is: sessionKey + ":" + CRC32(ciphertext) + ":" + INSTANCE_TOKEN
     *
     * Three inputs are combined:
     *   1. sessionKey     — from the request (visible in logs and source)
     *   2. CRC32          — computed from the ciphertext bytes at runtime
     *   3. INSTANCE_TOKEN — captured at class load via System.nanoTime(); never
     *                       logged, not in any config, varies per JVM instance
     *
     * Reconstructing the selected transform index requires all three values.
     * INSTANCE_TOKEN is never observable through static source inspection.
     */
    private int selectTransform(String sessionKey, byte[] ciphertext) {
        CRC32 crc = new CRC32();
        crc.update(ciphertext);
        String composite = sessionKey + ":" + crc.getValue() + ":" + INSTANCE_TOKEN;

        int h = hashSeed;
        for (char c : composite.toCharArray()) {
            h = h * HASH_MULTIPLIER + c;
        }
        return (Math.abs(h) % TRANSFORM_COUNT) + 1;
    }
}
