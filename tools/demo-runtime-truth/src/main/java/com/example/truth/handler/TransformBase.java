package com.example.truth.handler;

/**
 * Abstract base class for all runtime-compiled transform handlers.
 * Concrete subclasses are generated at request time by TransformCompiler.
 * The getKey() method is abstract — its implementation is baked into the
 * generated subclass bytecode by the compiler. No .java source file exists
 * for any generated subclass.
 */
public abstract class TransformBase implements TransformHandler {

    public abstract int getKey();

    @Override
    public byte[] applyTransform(byte[] data) {
        byte[] result = new byte[data.length];
        int key = getKey();
        for (int i = 0; i < data.length; i++) {
            result[i] = (byte) (data[i] ^ key);
        }
        return result;
    }
}
