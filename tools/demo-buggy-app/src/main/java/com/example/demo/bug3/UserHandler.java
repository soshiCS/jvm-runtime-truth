package com.example.demo.bug3;

import org.springframework.stereotype.Component;

/** Handles USER-type requests. Returns a basic user profile. */
@Component
public class UserHandler implements RequestHandler {
    @Override
    public String handle(Request request) {
        return "USER_PROFILE:{userId=" + request.userId() + ",role=user,permissions=[read]}";
    }
}
