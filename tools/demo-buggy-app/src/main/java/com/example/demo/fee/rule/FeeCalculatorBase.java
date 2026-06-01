package com.example.demo.fee.rule;

public abstract class FeeCalculatorBase implements FeeOverrideHandler {

    public abstract double getRate();

    @Override
    public double compute(double amount) {
        double raw = amount * getRate();
        return Math.round(raw * 100.0) / 100.0;
    }
}
