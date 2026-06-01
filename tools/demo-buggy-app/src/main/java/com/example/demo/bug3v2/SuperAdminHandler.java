package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class SuperAdminHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "SUPER_ADMIN"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=super-admin,permissions=[read,write,delete,manage,admin,superadmin]}";
    }
}
