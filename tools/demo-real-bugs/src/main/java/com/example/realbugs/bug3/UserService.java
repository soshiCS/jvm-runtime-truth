package com.example.realbugs.bug3;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

/**
 * Bug 3 — Hibernate proxy substitution.
 *
 * Source: https://github.com/hibernate/hibernate-orm/issues/7169
 *
 * Pattern: Within a single @Transactional session, loading an entity via a
 * @ManyToOne relationship (Post.getUser()) creates a HibernateProxy in the
 * session cache. A subsequent findById() for the same entity ID returns that
 * HibernateProxy rather than the initialized User.
 *
 * The visible symptom: code that checks `entity.getClass() == User.class`
 * or casts to a subclass fails because the runtime type is
 * User$HibernateProxy$<hash>, not User.
 *
 * Runtime Truth detects this because callsite_target records for
 * UserRepository.findById() show User$HibernateProxy$<hash> as the
 * concrete return type — invisible at the source level.
 */
@Service
public class UserService {

    private static final Logger log = LoggerFactory.getLogger(UserService.class);

    private final UserRepository userRepository;

    public UserService(UserRepository userRepository) {
        this.userRepository = userRepository;
    }

    /**
     * Seeds test data: one user with two posts.
     */
    @Transactional
    public Long seedData() {
        User user = new User("Alice");
        Post p1 = new Post("Post A", user);
        Post p2 = new Post("Post B", user);
        user.getPosts().add(p1);
        user.getPosts().add(p2);
        userRepository.save(user);
        return user.getId();
    }

    /**
     * Inspects the runtime class of the object returned by findById()
     * after a JOIN FETCH has already loaded the same entity in a different session.
     *
     * Step 1: findUserWithPosts() — loads User with posts via JOIN FETCH.
     *         This session is committed and closed.
     * Step 2 (separate @Transactional): Load a Post so its .getUser() reference
     *         creates a proxy in the session cache.
     * Step 3: Call findById() for the same user in that second session.
     *         Hibernate returns the proxy from the cache, not the real entity.
     */
    @Transactional(readOnly = true)
    public String inspectViaJoinFetch(Long userId) {
        // Step 1: load via JOIN FETCH (primes the session-level first-level cache)
        User joinFetchUser = userRepository.findUserWithPosts(userId);
        String joinFetchClass = joinFetchUser.getClass().getName();
        log.info("[Bug3] findUserWithPosts → class: {}", joinFetchClass);

        // Step 2: Now call findById inside the SAME transaction/session.
        // Hibernate returns the already-cached object from the first-level cache.
        // The interesting case is when the cache has a proxy placeholder.
        User findByIdUser = userRepository.findById(userId).orElseThrow();
        String findByIdClass = findByIdUser.getClass().getName();
        log.info("[Bug3] findById        → class: {}", findByIdClass);

        return joinFetchClass + " vs " + findByIdClass;
    }

    /**
     * The proxy substitution scenario: load a Post first (which creates a User proxy
     * in the session cache via @ManyToOne), then call findById for that User.
     * findById returns the proxy, not the initialized entity.
     */
    @Transactional(readOnly = true)
    public String inspectViaPostNavigation(Long userId) {
        // Load a post belonging to userId — this puts a User PROXY into the L1 cache
        // (the @ManyToOne is LAZY, so Hibernate creates a HibernateProxy placeholder)
        Post post = userRepository.findById(userId)
                .map(u -> u.getPosts().isEmpty() ? null : u.getPosts().get(0))
                .orElse(null);

        if (post == null) {
            return "no posts found";
        }

        // Accessing post.getUser() returns the L1 cache proxy
        User userViaPost = post.getUser();
        String viaPostClass = userViaPost.getClass().getName();
        log.info("[Bug3] post.getUser()  → class: {}", viaPostClass);

        // findById in the same session returns whatever is in the L1 cache:
        // if it's a proxy, you get the proxy back
        User findByIdUser = userRepository.findById(userId).orElseThrow();
        String findByIdClass = findByIdUser.getClass().getName();
        log.info("[Bug3] findById        → class: {}", findByIdClass);

        boolean sameInstance = (userViaPost == findByIdUser);
        boolean isProxy = findByIdClass.contains("HibernateProxy") || findByIdClass.contains("$");

        return String.format(
            "post.getUser()=%s | findById=%s | sameInstance=%b | isProxy=%b",
            viaPostClass, findByIdClass, sameInstance, isProxy
        );
    }
}
