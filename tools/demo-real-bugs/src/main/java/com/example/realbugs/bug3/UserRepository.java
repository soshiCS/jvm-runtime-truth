package com.example.realbugs.bug3;

import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.data.jpa.repository.Query;
import org.springframework.data.repository.query.Param;

public interface UserRepository extends JpaRepository<User, Long> {

    /**
     * JOIN FETCH loads User + posts eagerly in one query.
     * This populates the Hibernate session cache with a fully-initialized User.
     *
     * Subsequent findById() in the SAME session returns the cached object —
     * but if the first load happened via a different session/proxy path,
     * findById can return a User$HibernateProxy$<hash> instead of User.
     *
     * Bug trigger: call findUserWithPosts() then findById() in the same
     * @Transactional scope but with the session cache primed in a certain way.
     */
    @Query("SELECT u FROM User u JOIN FETCH u.posts WHERE u.id = :id")
    User findUserWithPosts(@Param("id") Long id);
}
