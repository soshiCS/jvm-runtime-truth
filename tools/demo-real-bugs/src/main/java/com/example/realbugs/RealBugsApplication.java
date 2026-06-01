package com.example.realbugs;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.retry.annotation.EnableRetry;

@SpringBootApplication
@EnableRetry
public class RealBugsApplication {
    public static void main(String[] args) {
        SpringApplication.run(RealBugsApplication.class, args);
    }
}
