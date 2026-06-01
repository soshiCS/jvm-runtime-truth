package com.example.proxydemo;

public record CallRecord(
    String callerMethod,
    String callerTxName,
    String saveAuditEntryTxName,
    String callExpression
) {
    public boolean proxyBypassed() {
        return callerTxName.equals(saveAuditEntryTxName);
    }
}
