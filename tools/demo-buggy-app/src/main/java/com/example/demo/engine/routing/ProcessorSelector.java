package com.example.demo.engine.routing;

import org.springframework.stereotype.Component;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

/**
 * Selects the processor implementation for a given (segment, operation) routing pair.
 *
 * Uses a polynomial rolling hash with a configurable seed for deterministic,
 * reproducible routing across all account segments and operation types.
 * The seed is loaded from routing/selector-config.properties at startup.
 */
@Component
public class ProcessorSelector {

    private static final String PROC_CLASS_PREFIX = "com.example.demo.engine.proc.Processor";
    private static final int HASH_MULTIPLIER = 31;

    private final int hashSeed;

    public ProcessorSelector() throws IOException {
        Properties cfg = new Properties();
        try (InputStream in = getClass().getClassLoader()
                .getResourceAsStream("routing/selector-config.properties")) {
            if (in == null) throw new IOException("routing/selector-config.properties not found");
            cfg.load(in);
        }
        this.hashSeed = Integer.parseInt(cfg.getProperty("routing.hash.seed"));
    }

    /**
     * Returns the fully qualified class name of the processor for the given
     * segment code and operation type.
     *
     * The processor index is computed by applying a polynomial rolling hash
     * over the segment code followed by the operation string, then mapping
     * the result to the range [1, 50] via modulo arithmetic.
     */
    public String select(String segment, String operation) {
        int h = hashSeed;
        for (char c : segment.toCharArray()) {
            h = h * HASH_MULTIPLIER + c;
        }
        for (char c : operation.toCharArray()) {
            h = h * HASH_MULTIPLIER + c;
        }
        int processorIndex = (Math.abs(h) % 50) + 1;
        return String.format("%s%03d", PROC_CLASS_PREFIX, processorIndex);
    }
}
