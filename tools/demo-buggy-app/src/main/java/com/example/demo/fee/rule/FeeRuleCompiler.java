package com.example.demo.fee.rule;

import net.bytebuddy.ByteBuddy;
import net.bytebuddy.NamingStrategy;
import net.bytebuddy.dynamic.loading.ClassLoadingStrategy;
import net.bytebuddy.implementation.FixedValue;
import net.bytebuddy.matcher.ElementMatchers;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

/**
 * Compiles a FeeOverrideHandler for a given (tier, operation) pair by:
 *   1. Computing a profile index via a polynomial rolling hash of tier+operation.
 *   2. Looking up the override rate for that profile in rate-profiles.properties.
 *   3. Generating a concrete subclass of FeeCalculatorBase with the rate baked in
 *      as a constant using ByteBuddy.
 *
 * The generated class name includes the profile index (e.g. FeeCalculatorBase$Profile009$...)
 * so that monitoring tools can correlate a running handler back to its config entry.
 */
@Component
public class FeeRuleCompiler {

    private static final String PROFILES_CONFIG = "fee-config/rate-profiles.properties";
    private static final String SELECTOR_CONFIG = "fee-config/selector-config.properties";
    private static final int    HASH_MULTIPLIER = 31;
    private static final int    PROFILE_COUNT   = 50;

    private final int        hashSeed;
    private final Properties profileRates;

    public FeeRuleCompiler() throws IOException {
        Properties selector = new Properties();
        try (InputStream in = FeeRuleCompiler.class.getClassLoader()
                .getResourceAsStream(SELECTOR_CONFIG)) {
            if (in == null) throw new IOException(SELECTOR_CONFIG + " not found");
            selector.load(in);
        }
        this.hashSeed = Integer.parseInt(selector.getProperty("profile.hash.seed"));

        this.profileRates = new Properties();
        try (InputStream in = FeeRuleCompiler.class.getClassLoader()
                .getResourceAsStream(PROFILES_CONFIG)) {
            if (in == null) throw new IOException(PROFILES_CONFIG + " not found");
            profileRates.load(in);
        }
    }

    public FeeOverrideHandler compile(String tier, String operation) throws Exception {
        int profileIndex = selectProfile(tier, operation);
        String profileKey = String.format("profile.%03d", profileIndex);
        double rate = Double.parseDouble(profileRates.getProperty(profileKey));

        String profileTag = String.format("Profile%03d", profileIndex);

        Class<?> generated = new ByteBuddy()
                .with(new NamingStrategy.SuffixingRandom(profileTag))
                .subclass(FeeCalculatorBase.class)
                .method(ElementMatchers.named("getRate"))
                .intercept(FixedValue.value(rate))
                .make()
                .load(FeeRuleCompiler.class.getClassLoader(),
                        ClassLoadingStrategy.Default.INJECTION)
                .getLoaded();

        return (FeeOverrideHandler) generated.getDeclaredConstructor().newInstance();
    }

    /**
     * Maps a (tier, operation) pair to a profile index in [1, PROFILE_COUNT].
     * Uses a polynomial rolling hash with configurable seed for reproducible
     * profile assignment across all service instances.
     */
    private int selectProfile(String tier, String operation) {
        int h = hashSeed;
        for (char c : tier.toCharArray()) {
            h = h * HASH_MULTIPLIER + c;
        }
        for (char c : operation.toCharArray()) {
            h = h * HASH_MULTIPLIER + c;
        }
        return (Math.abs(h) % PROFILE_COUNT) + 1;
    }
}
