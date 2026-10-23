/* Exercise the actual orbital model, including reachable routes on each
 * viewport. Including the implementation keeps private rules private. */
#include "../components/apps/minigames/games/quantum.c"
#include <assert.h>

int main(void)
{
    mg_session_t *session = mg_create(&mg_quantum, 42, 0);
    assert(session);
    mg_input(session, MG_PRESS, 500, 500, 0);
    mg_advance(session, 240, true);
    mg_advance(session, 1000, false);
    quantum_model_t *paused = &((quantum_state_t *)mg_state(session))->model;
    assert(paused->orbiting && !paused->held && !paused->charge);
    int elapsed = paused->elapsed;
    mg_advance(session, 2000, true);
    assert(paused->elapsed == elapsed);
    mg_destroy(session);
    quantum_state_t *s = calloc(1, sizeof(*s));
    assert(s);
    start(s, 42);
    mg_input_t event = {.kind = MG_PRESS};
    input(s, &event);
    for (int i = 0; i < 40; i++) step(s, 16);
    assert(s->model.charge >= 80 && s->model.orbiting);
    event.kind = MG_RELEASE;
    event.cancelled = true;
    input(s, &event);
    assert(s->model.orbiting && !s->model.held && !s->model.charge);
    event.kind = MG_PRESS;
    event.cancelled = false;
    input(s, &event);
    for (int i = 0; i < 40; i++) step(s, 16);
    event.kind = MG_RELEASE;
    input(s, &event);
    assert(!s->model.orbiting && !s->model.held);
    impulse(&s->model);
    assert(s->model.energy == 60);
    impulse(&s->model);
    assert(s->model.energy == 60);
    fail(&s->model);
    assert(s->model.lives == 2 && s->model.orbiting);
    fail(&s->model);
    fail(&s->model);
    assert(s->model.done && s->model.lives == 0);

    /* Search launch angles, not arbitrary teleports: every captured world
     * must admit a real flight using the same fixed step as the device. */
    const int heights[] = {448, 450, 466, 540};
    for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); h++)
        for (unsigned seed = 1; seed <= 24; seed++)
        {
            quantum_model_t route;
            init_model(&route, seed, heights[h]);
            for (int jump = 0; jump < Q_GOAL; jump++)
            {
                bool found = false;
                for (int charge = 100; charge >= 0 && !found; charge -= 25)
                    for (int phase = 0; phase < 1024 && !found; phase += 8)
                    {
                        quantum_model_t candidate = route;
                        candidate.phase = phase;
                        candidate.charge = charge;
                        orbit_position(&candidate);
                        launch(&candidate);
                        for (int n = 0; n < 260 && !candidate.orbiting && !candidate.done; n++) model_step(&candidate, 16);
                        if (candidate.jumps == jump + 1)
                        {
                            route = candidate;
                            found = true;
                        }
                    }
                assert(found);
            }
            assert(route.done && route.jumps == Q_GOAL && route.lives == 3);
            assert(route.elapsed < MG_MAX_DURATION_MS);
        }
    free(s);
    puts("PASS: quantum controls, impulse cooldown, shields and 96 complete eight-planet routes");
    return 0;
}
