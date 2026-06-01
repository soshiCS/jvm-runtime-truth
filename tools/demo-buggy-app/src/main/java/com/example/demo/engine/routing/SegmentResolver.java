package com.example.demo.engine.routing;

import org.springframework.stereotype.Component;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

@Component
public class SegmentResolver {

    private final Properties registry = new Properties();

    public SegmentResolver() throws IOException {
        try (InputStream in = getClass().getClassLoader()
                .getResourceAsStream("routing/segment-registry.properties")) {
            if (in == null) throw new IOException("routing/segment-registry.properties not found");
            registry.load(in);
        }
    }

    public String resolve(String accountId) {
        int lastDash = accountId.lastIndexOf('-');
        if (lastDash < 0) throw new IllegalArgumentException("Malformed accountId: " + accountId);
        String suffix = accountId.substring(lastDash + 1);
        String segment = registry.getProperty(suffix);
        if (segment == null) throw new IllegalArgumentException("Unknown account suffix: " + suffix);
        return segment;
    }
}
