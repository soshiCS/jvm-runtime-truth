package com.example.truth;

import com.example.truth.model.DecryptContext;
import com.example.truth.model.DecryptRequest;
import com.example.truth.model.DecryptResult;
import org.springframework.web.bind.annotation.*;

import java.nio.charset.StandardCharsets;
import java.util.HexFormat;

@RestController
@RequestMapping("/api/decrypt")
public class DecryptController {

    private final DecryptPipeline pipeline;

    public DecryptController(DecryptPipeline pipeline) {
        this.pipeline = pipeline;
    }

    @PostMapping
    public DecryptResult decrypt(@RequestParam String sessionKey,
                                  @RequestBody DecryptRequest request) throws Exception {
        byte[] ciphertext = HexFormat.of().parseHex(request.ciphertext());

        System.out.printf("[DECRYPT] Processing sessionKey=%s ciphertext=%s%n",
                sessionKey, request.ciphertext());

        DecryptContext ctx = new DecryptContext(sessionKey, ciphertext);
        byte[] plaintext = pipeline.execute(ctx);

        System.out.println("[DECRYPT] Pipeline completed — inspecting result");

        if (plaintext != null && plaintext.length > 0 && allPrintable(plaintext)) {
            String output = new String(plaintext, StandardCharsets.US_ASCII);
            System.out.printf("[DECRYPT] Result: status=OK output=%s%n", output);
            return new DecryptResult("OK", output, sessionKey);
        }

        String raw = plaintext != null ? HexFormat.of().formatHex(plaintext) : "";
        System.out.printf("[DECRYPT] Result: status=DECRYPTION_FAILED output=%s%n", raw);
        return new DecryptResult("DECRYPTION_FAILED", raw, sessionKey);
    }

    private static boolean allPrintable(byte[] data) {
        for (byte b : data) {
            int v = b & 0xFF;
            if (v < 0x20 || v >= 0x7F) return false;
        }
        return true;
    }
}
