package com.example.demo.fee.routing;

import org.springframework.stereotype.Component;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

@Component
public class AccountTierResolver {

    private static final String CONFIG_RESOURCE = "fee-config/tier-mapping.properties";
    private final Properties tierMap;

    public AccountTierResolver() throws IOException {
        tierMap = new Properties();
        try (InputStream in = AccountTierResolver.class.getClassLoader()
                .getResourceAsStream(CONFIG_RESOURCE)) {
            if (in == null) throw new IOException(CONFIG_RESOURCE + " not found");
            tierMap.load(in);
        }
    }

    public String resolve(String accountId) {
        // Extract the first token before the first hyphen or digit run
        // e.g. "ACC-PREM-8821" → prefix segment "PREM"
        String[] parts = accountId.split("[-_]");
        for (String part : parts) {
            String tier = tierMap.getProperty(part.toUpperCase());
            if (tier != null) return tier;
        }
        return "standard";
    }
}
