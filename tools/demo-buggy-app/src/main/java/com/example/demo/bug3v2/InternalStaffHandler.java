package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class InternalStaffHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "INTERNAL_STAFF"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=internal-staff,permissions=[read,write,internal]}";
    }
}
