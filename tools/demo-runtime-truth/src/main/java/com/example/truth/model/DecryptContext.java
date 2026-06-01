package com.example.truth.model;

public record DecryptContext(String sessionKey, byte[] ciphertext) {}
