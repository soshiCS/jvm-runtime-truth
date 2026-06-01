package com.example.demo.bug3;

import org.springframework.stereotype.Component;

/**
 * Handles GUEST-type requests.
 *
 * BUG: guests should receive a restricted profile with read-only access to
 * public resources only. This implementation was copy-pasted from AdminHandler
 * and the permissions list was never changed — guests receive full admin
 * permissions including write, delete, and manage.
 *
 * No exception is thrown. The response looks structurally correct.
 * The symptom is a privilege escalation: guest sessions have admin rights.
 *
 * Causality reveals the polymorphic dispatch chain via blocked_multi_target:
 * RequestRouter.route → [UserHandler, AdminHandler, GuestHandler]
 * and shows GuestHandler.handle as the actual target for GUEST requests.
 */
@Component
public class GuestHandler implements RequestHandler {
    @Override
    public String handle(Request request) {
        // BUG IS HERE: should return role=guest,permissions=[read:public]
        // Copied from AdminHandler without adjusting the role or permission list.
        return "GUEST_PROFILE:{userId=" + request.userId() + ",role=guest,permissions=[read,write,delete,manage]}";
    }
}
