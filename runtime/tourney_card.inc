/* (2026-11-02) Tournaments on screen, without F1 (included by menu.cpp): the player's own tournament (lobby 1.7,
 * tools/net/tournament.py) as cards over the game, driven by the controller.
 *
 *   - a small line at the top right while the player is in a tournament (not during a fight): what to do now;
 *   - the tournament screen (F3, or hold - (Minus) for about a second; B / Esc closes it): sign up / withdraw, check in,
 *     the set (opponent, round, score), the stage strike / ban / pick with big tiles (d-pad / arrows + A / Enter), the
 *     next set, the place; Y / Tab: the player's path in the bracket and the standings;
 *   - it opens by itself when the player must act: the check-in, a new set, its stage turn (never during a fight), and
 *     closes by itself a few seconds after the stage is chosen ("go to Smash, pick your fighter, press start").
 * While the screen is open the game gets no input (menu_input_blocked), as with F1; after it closes, nothing until every
 * button is released (g_guard). Every state says on the card what the player does next (the owner's rule: the friends
 * manage alone). The lobby does the rest: pairing, rules, results (net_session.c holds the set-up until the set may start). */

static NetTourney g_tc;                                          /* (render thread) the last state, read each frame */
static int g_tc_auto, g_tc_sel, g_tc_tab, g_tc_confirm, g_tc_list_sel;
static ULONGLONG g_tc_close_at, g_tc_banner_until, g_tc_opened_at;
static char g_tc_banner[240];
static char g_tc_sig_turn[96], g_tc_sig_set[64];
static int g_tc_checkin_for, g_tc_invite_for;

extern "C" int menu_tourney_modal(void) { return g_enabled > 0 && g_tc_modal; }
#ifdef _WIN32
extern "C" int ra_fight_on(void);                               /* rollback/model/runahead.c: the FighterManager's ready-go byte */
static int tc_fight_on(void) { return ra_fight_on(); }
#else
static int tc_fight_on(void) { return 0; }
#endif
static int tc_is_stage_phase(const NetTourney &t) { return t.set >= 0 && (!strcmp(t.phase, "strike") || !strcmp(t.phase, "ban") || !strcmp(t.phase, "pick")); }
static int tc_struck(const NetTourney &t, const char *s) { for (int i = 0; i < t.nstruck; i++) if (!strcmp(t.struck[i], s)) return 1; return 0; }
static const char *tc_round_name(char br, int round, char *out, int n) {
    if (br == 'G') snprintf(out, (size_t)n, round == 2 ? "Grand final reset" : "Grand final");
    else snprintf(out, (size_t)n, "%s round %d", br == 'L' ? "Losers" : "Winners", round);
    return out;
}
static void tc_open(int on, int automatic) {
    if (on && !g_tc_modal) { g_tc_opened_at = GetTickCount64(); g_tc_confirm = 0; }
    if (!on && g_tc_modal) { InterlockedExchange(&g_guard, 1); g_closed_at = g_frame; }      /* (the button that closed it never reaches the game) */
    InterlockedExchange(&g_tc_modal, on);
    g_tc_auto = on && automatic;
    g_tc_close_at = 0;
    if (on) { printf("menu: tournament screen opened%s\n", automatic ? " by itself" : ""); fflush(stdout); }
}
static void tc_first_free_stage(void) {
    for (int k = 0; k < g_tc.npool; k++) { int i = (g_tc_sel + k) % (g_tc.npool ? g_tc.npool : 1); if (!tc_struck(g_tc, g_tc.pool[i])) { g_tc_sel = i; return; } }
}
static void tc_move_stage(int step) {
    if (g_tc.npool <= 0) return;
    for (int k = 1; k <= g_tc.npool; k++) {
        int i = ((g_tc_sel + step * k) % g_tc.npool + g_tc.npool) % g_tc.npool;
        if (!tc_struck(g_tc, g_tc.pool[i])) { g_tc_sel = i; return; }
    }
}
/* the open tournaments of the list the player may sign up for */
static int tc_open_list(int idx[NET_T_LIST]) {
    int n = 0;
    for (int i = 0; i < g_tc.n; i++) if (!strcmp(g_tc.list[i].state, "signup") || !strcmp(g_tc.list[i].state, "checkin")) idx[n++] = i;
    return n;
}

/* every frame, before anything is drawn: the state, the toggle, what opens the screen by itself, the controls */
static void tc_tick(uint64_t b, uint64_t down) {
    ULONGLONG now = GetTickCount64();
    net_tourney_peek(&g_tc);
    {   /* F3 (window thread) or - held about a second (not with +: that is F1's combination) */
        static ULONGLONG t0; static int done;
        if ((b & BIT(B_MINUS)) && !(b & BIT(B_PLUS)) && !g_open) {
            if (!t0) t0 = now;
            else if (now - t0 >= 800 && !done) { done = 1; InterlockedIncrement(&g_tc_toggle_req); }
        } else { t0 = 0; done = 0; }
    }
    for (LONG t = InterlockedExchange(&g_tc_toggle_req, 0); t > 0; t--) {
        if (!g_tc.known) { menu_popup("Tournaments", "Not connected to the tournament server.\nF1 > Online > Tournaments: ON, then Restart the game now."); continue; }
        tc_open(!g_tc_modal, 0);
        if (g_tc_modal) tc_first_free_stage();
    }
    if (g_open && g_tc_modal) tc_open(0, 0);            /* (F1 wins) */
    if (!g_tc.known) { if (g_tc_modal) tc_open(0, 0); return; }
    int fight = tc_fight_on();
    /* what opens the screen by itself (once per reason; never during a fight nor over F1) */
    char sig[96];
    if (tc_is_stage_phase(g_tc) && g_tc.my_turn) {
        snprintf(sig, sizeof sig, "%d/%d/%s/%d", g_tc.set, g_tc.game, g_tc.phase, g_tc.nstruck);
        if (strcmp(sig, g_tc_sig_turn) && !fight && !g_open) {
            snprintf(g_tc_sig_turn, sizeof g_tc_sig_turn, "%s", sig);
            if (!g_tc_modal) tc_open(1, 1);
            tc_first_free_stage();
        }
    }
    if (g_tc.set >= 0) {
        snprintf(sig, sizeof sig, "%d/%d/%d", g_tc.own_id, g_tc.set, g_tc.game);
        if (strcmp(sig, g_tc_sig_set) && !fight && !g_open) { snprintf(g_tc_sig_set, sizeof g_tc_sig_set, "%s", sig); if (!g_tc_modal) tc_open(1, 1); g_tc_tab = 0; }
    }
    if (g_tc.own_id && !strcmp(g_tc.own_state, "checkin") && !g_tc.own_in && g_tc_checkin_for != g_tc.own_id && !fight && !g_open) {
        g_tc_checkin_for = g_tc.own_id;
        if (!g_tc_modal) tc_open(1, 1);
        g_tc_tab = 0;
    }
    {   int idx[NET_T_LIST], n = tc_open_list(idx);      /* a tournament open for sign-ups that the player is not in: a note at the top, once */
        for (int k = 0; k < n; k++) {
            const NetTourneyItem &it = g_tc.list[idx[k]];
            if (!it.signed_up && it.id > g_tc_invite_for && !g_tc.own_id) {
                g_tc_invite_for = it.id;
                snprintf(g_tc_banner, sizeof g_tc_banner, "Tournament open: %s (%d players).\nTo sign up: press F3, or hold - (Minus) for a second.", it.name, it.players);
                g_tc_banner_until = now + 15000;
            }
        }
    }
    /* opened by itself for a stage turn: when the stage is chosen, the card says what follows for 4 s, then closes */
    if (g_tc_modal && g_tc_auto && g_tc.set >= 0 && (!strcmp(g_tc.phase, "chars") || !strcmp(g_tc.phase, "go")) && !g_tc_close_at && now - g_tc_opened_at > 1500)
        g_tc_close_at = now + 4000;
    if (g_tc_modal && g_tc_close_at && now >= g_tc_close_at) tc_open(0, 0);
    if (g_tc_modal && fight) tc_open(0, 0);              /* (a fight began: the game's input is the player's) */
    if (!g_tc_modal) { InterlockedExchange(&g_tc_key, 0); return; }
    /* the controls of the open screen */
    int vk = (int)InterlockedExchange(&g_tc_key, 0);
    int k_a = (down & BIT(B_A)) || vk == VK_RETURN || vk == VK_SPACE, k_b = (down & BIT(B_B)) || vk == VK_ESCAPE;
    int k_x = (down & BIT(B_X)) || vk == 'X', k_y = (down & BIT(B_Y)) || vk == VK_TAB;
    int k_l = (down & BIT(B_LEFT)) || vk == VK_LEFT, k_r = (down & BIT(B_RIGHT)) || vk == VK_RIGHT;
    int k_u = (down & BIT(B_UP)) || vk == VK_UP, k_d = (down & BIT(B_DOWN)) || vk == VK_DOWN;
    if (now - g_tc_opened_at < 250) k_a = k_b = k_x = k_y = 0;     /* (the press that opened it) */
    if (k_b) { if (g_tc_confirm) g_tc_confirm = 0; else tc_open(0, 0); return; }
    if (k_y) { g_tc_tab = !g_tc_tab; return; }
    if (g_tc_tab) return;
    if (tc_is_stage_phase(g_tc)) {
        const int cols = 3;
        if (k_l) tc_move_stage(-1);
        if (k_r) tc_move_stage(1);
        if (k_u) tc_move_stage(-cols);
        if (k_d) tc_move_stage(cols);
        if (k_a && g_tc.my_turn && !g_tc.pending && g_tc_sel < g_tc.npool && !tc_struck(g_tc, g_tc.pool[g_tc_sel])) {
            printf("menu: tournament: %s %s\n", g_tc.phase, g_tc.pool[g_tc_sel]); fflush(stdout);
            net_tourney_stage(g_tc.own_id, g_tc.set, g_tc.pool[g_tc_sel]);
            tc_move_stage(1);
        }
        return;
    }
    if (g_tc.set >= 0) { if (k_a) tc_open(0, 0); return; }      /* (the set's card: A closes it, to go to Smash) */
    if (g_tc.own_id && !strcmp(g_tc.own_state, "checkin") && !g_tc.own_in) {
        if (k_a && !g_tc.pending) { net_tourney_simple(13, g_tc.own_id); printf("menu: tournament: check in\n"); fflush(stdout); }
        if (k_x) g_tc_confirm = 1;
        else if (g_tc_confirm && k_a) { net_tourney_simple(2, g_tc.own_id); g_tc_confirm = 0; }
        return;
    }
    if (g_tc.own_id && strcmp(g_tc.own_state, "running") && strcmp(g_tc.own_state, "done")) {      /* signed up (or checked in): withdraw */
        if (k_x && !g_tc_confirm) g_tc_confirm = 1;
        else if (g_tc_confirm && k_a && !g_tc.pending) { net_tourney_simple(2, g_tc.own_id); g_tc_confirm = 0; printf("menu: tournament: withdraw\n"); fflush(stdout); }
        else if (k_a) tc_open(0, 0);
        return;
    }
    if (!g_tc.own_id || !strcmp(g_tc.own_state, "done")) {     /* the list: sign up */
        int idx[NET_T_LIST], n = tc_open_list(idx);
        if (n) {
            if (k_u) g_tc_list_sel = (g_tc_list_sel + n - 1) % n;
            if (k_d) g_tc_list_sel = (g_tc_list_sel + 1) % n;
            if (g_tc_list_sel >= n) g_tc_list_sel = 0;
            if (k_a && !g_tc.pending) { const NetTourneyItem &it = g_tc.list[idx[g_tc_list_sel]]; net_tourney_simple(1, it.id); printf("menu: tournament: sign up for %d\n", it.id); fflush(stdout); }
        } else if (k_a) tc_open(0, 0);
        return;
    }
    if (k_a) tc_open(0, 0);
}
static int tc_visible(void) {
    if (g_tc_modal || GetTickCount64() < g_tc_banner_until) return 1;
    return g_tc.known && g_tc.own_id && strcmp(g_tc.own_state, "done") && !g_open;
}

/* ---- drawing */
static const ImVec4 TC_GOLD(1.0f, 0.78f, 0.25f, 1.0f), TC_DIM(0.65f, 0.65f, 0.65f, 1.0f), TC_GREEN(0.45f, 0.9f, 0.45f, 1.0f), TC_RED(1.0f, 0.45f, 0.4f, 1.0f);
static void tc_hint(const char *text) { ImGui::Spacing(); ImGui::TextColored(TC_DIM, "%s", text); }
/* the one-line state at the top right (no input) */
static void tc_status_line(char *out, int n) {
    const NetTourney &t = g_tc;
    out[0] = 0;
    if (t.set >= 0) {
        if (tc_is_stage_phase(t)) snprintf(out, (size_t)n, t.my_turn ? "YOUR TURN: %s a stage (F3)" : "%s is choosing the stage", t.my_turn ? t.phase : t.opp);
        else if (t.feature[0]) snprintf(out, (size_t)n, "vs %s (%d-%d) ON STREAM: Battle Arena %s", t.opp, t.mine, t.theirs, t.feature);
        else snprintf(out, (size_t)n, "vs %s (%d-%d) on %s: go to Smash, pick your fighter, press start", t.opp, t.mine, t.theirs, net_tourney_stage_name(t.stage));
    } else if (!strcmp(t.own_state, "checkin")) snprintf(out, (size_t)n, t.own_in ? "Checked in: the bracket starts soon" : "CHECK-IN OPEN: press F3 to check in");
    else if (!strcmp(t.own_state, "signup")) snprintf(out, (size_t)n, "Signed up: %s (check-in comes later)", t.own_name);
    else if (!strcmp(t.own_state, "running")) {
        if (!strcmp(t.status, "out")) snprintf(out, (size_t)n, "Out of the tournament (place %d)", t.place);
        else if (!strcmp(t.status, "dq")) snprintf(out, (size_t)n, "Disqualified");
        else if (t.next_br) snprintf(out, (size_t)n, "Next set: vs %s (keep the game open)", t.next_who);
        else snprintf(out, (size_t)n, "Waiting for your next set");
    }
}
static void tc_draw_set(void) {
    const NetTourney &t = g_tc;
    char rn[48];
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(TC_GOLD, "vs %s", t.opp);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Text("%s  -  best of %d  -  game %d  -  score %d - %d", tc_round_name(t.br, t.round, rn, sizeof rn), t.set_bo ? t.set_bo : t.need * 2 - 1, t.game, t.mine, t.theirs);
    ImGui::Separator();
    if (tc_is_stage_phase(t)) {
        const char *verb = !strcmp(t.phase, "strike") ? "STRIKE" : !strcmp(t.phase, "ban") ? "BAN" : "PICK";
        if (t.my_turn) {
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(TC_GOLD, "YOUR TURN: %s %s", verb, !strcmp(t.phase, "pick") ? "the stage" : t.left > 1 ? "stages" : "a stage");
            ImGui::SetWindowFontScale(1.0f);
            if (!strcmp(t.phase, "strike") || !strcmp(t.phase, "ban")) ImGui::Text("%d left to %s this turn.", t.left, !strcmp(t.phase, "strike") ? "strike" : "ban");
        } else ImGui::Text("%s is choosing (%s)...", t.opp, !strcmp(t.phase, "strike") ? "striking stages" : !strcmp(t.phase, "ban") ? "banning stages" : "picking the stage");
        if (t.turn_secs >= 0) { ImGui::SameLine(); ImGui::TextColored(t.turn_secs <= 10 ? TC_RED : TC_DIM, "  %d s", t.turn_secs); }
        ImGui::Spacing();
        const int cols = 3;
        float w = ImGui::GetFontSize() * 11.0f, h = ImGui::GetFontSize() * 2.6f;
        for (int i = 0; i < t.npool; i++) {
            if (i % cols) ImGui::SameLine();
            int struck = tc_struck(t, t.pool[i]), sel = t.my_turn && i == g_tc_sel;
            ImVec4 bg = struck ? ImVec4(0.18f, 0.18f, 0.18f, 0.9f) : sel ? ImVec4(0.85f, 0.45f, 0.1f, 1.0f) : ImVec4(0.22f, 0.26f, 0.36f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, bg); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg); ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, struck ? ImVec4(0.5f, 0.5f, 0.5f, 1) : ImVec4(1, 1, 1, 1));
            char label[64]; snprintf(label, sizeof label, "%s%s##tc%d", net_tourney_stage_name(t.pool[i]), struck ? "\n(out)" : "", i);
            ImGui::Button(label, ImVec2(w, h));
            ImGui::PopStyleColor(4);
        }
        tc_hint(t.my_turn ? "d-pad / arrows: choose   A / Enter: " : "Wait for the other player.   ");
        if (t.my_turn) { ImGui::SameLine(0, 0); ImGui::TextColored(TC_DIM, "%s   B: close   Y: bracket", !strcmp(t.phase, "strike") ? "strike" : !strcmp(t.phase, "ban") ? "ban" : "pick"); }
        if (t.turn_secs >= 0 && t.my_turn) tc_hint("If the time runs out, the first stage left is chosen for you.");
        return;
    }
    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("Stage: "); ImGui::SameLine(0, 0); ImGui::TextColored(TC_GOLD, "%s", t.stage[0] ? net_tourney_stage_name(t.stage) : "-");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    if (t.feature[0]) {                                  /* (on stream: the set is played in a Battle Arena, the others watch) */
        ImGui::TextColored(TC_GOLD, "This set is ON STREAM: play it in the Battle Arena with the code %s.", t.feature);
        ImGui::Text("Battle Arenas must be ON (F1 > Experimental). Online > Battle Arena > enter the code, then fight there as usual.");
        ImGui::TextColored(TC_DIM, "The result counts for the set by itself.");
        tc_hint("A / B: close   Y: bracket");
        return;
    }
    ImGui::TextColored(TC_GREEN, "Now: close this (A or B), go to Smash, pick your fighter and press start.");
    ImGui::Text("The match starts by itself with the tournament's rules (%d stocks, no items).", t.rule_stocks > 0 ? t.rule_stocks : 3);
    if (t.first == 2) ImGui::Text("You lost the last game: %s picks first; you see that fighter and can change yours.", t.opp);
    else if (t.first == 1) ImGui::Text("You won the last game: you pick your fighter first.");
    if (t.ready_them) ImGui::TextColored(TC_DIM, "%s is ready%s%s.", t.opp, t.their_chara_name[0] ? " with " : "", t.their_chara_name);
    tc_hint("A / B: close   Y: bracket");
}
static void tc_draw_bracket(void) {
    const NetTourney &t = g_tc;
    ImGui::TextColored(TC_GOLD, "Your path");
    int shown = 0;
    for (int i = 0; i < t.nsets; i++) {
        const NetTourneySet &s = t.sets[i];
        if (t.me < 0 || (s.p[0] != t.me && s.p[1] != t.me) || !strcmp(s.state, "skipped")) continue;
        int side = s.p[0] == t.me ? 0 : 1, o = s.p[1 - side];
        char rn[48]; tc_round_name(s.br, s.round, rn, sizeof rn);
        const char *on = o >= 0 && o < t.nent ? t.ent[o].name : o == -1 ? "(bye)" : "?";
        ImVec4 c = !strcmp(s.state, "done") || !strcmp(s.state, "forfeit") ? (s.winner == t.me ? TC_GREEN : TC_RED) : TC_GOLD;
        ImGui::TextColored(c, "%-22s vs %-20s %d - %d  %s", rn, on, s.score[side], s.score[1 - side],
                           !strcmp(s.state, "done") ? (s.winner == t.me ? "won" : "lost") : !strcmp(s.state, "bye") ? "bye" : s.state);
        shown++;
    }
    if (!shown) ImGui::TextColored(TC_DIM, "No sets yet.");
    if (t.next_br) ImGui::Text("Next: %s round %d vs %s", t.next_br == 'L' ? "Losers" : t.next_br == 'G' ? "Grand final" : "Winners", t.next_round, t.next_who);
    if (t.nplaces) {
        ImGui::Spacing(); ImGui::TextColored(TC_GOLD, "Standings");
        for (int i = 0; i < t.nplaces && i < 8; i++) { int e = t.places[i][1]; ImGui::Text("%d. %s", t.places[i][0], e >= 0 && e < t.nent ? t.ent[e].name : "?"); }
    }
    int live = 0; for (int i = 0; i < t.nsets; i++) if (!strcmp(t.sets[i].state, "playing") || !strcmp(t.sets[i].state, "ready")) live++;
    ImGui::TextColored(TC_DIM, "%d sets being played now.  The whole bracket: the tournament's web page.", live);
    tc_hint("Y / Tab: back   B: close");
}
static void tc_draw(ImGuiIO &io) {
    ULONGLONG now = GetTickCount64();
    const NetTourney &t = g_tc;
    if (!g_tc_modal) {
        if (now < g_tc_banner_until && !g_open) {            /* a tournament opened: once, at the top */
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.18f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.9f);
            ImGui::PushStyleColor(ImGuiCol_Border, TC_GOLD); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
            ImGui::Begin("##tcbanner", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored(TC_GOLD, "TOURNAMENT"); ImGui::Separator(); ImGui::TextUnformatted(g_tc_banner);
            ImGui::End(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
        }
        if (t.known && t.own_id && strcmp(t.own_state, "done") && !g_open && !tc_fight_on()) {     /* the small line at the top right */
            char line[200]; tc_status_line(line, sizeof line);
            if (line[0]) {
                ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - ImGui::GetFontSize(), io.DisplaySize.y * 0.02f + (S.stats ? io.DisplaySize.y * 0.12f : 0.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                ImGui::SetNextWindowBgAlpha(0.75f);
                ImGui::Begin("##tcline", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
                int act = (t.set >= 0 && t.my_turn) || (!strcmp(t.own_state, "checkin") && !t.own_in);
                ImGui::TextColored(act ? TC_GOLD : TC_DIM, "Tournament"); ImGui::SameLine(); ImGui::TextUnformatted(line);
                ImGui::End();
            }
        }
        return;
    }
    /* the tournament screen */
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x * 0.45f, 0), ImVec2(io.DisplaySize.x * 0.9f, io.DisplaySize.y * 0.9f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleColor(ImGuiCol_Border, TC_GOLD); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 1.4f, ImGui::GetFontSize() * 1.0f));
    ImGui::Begin("##tcscreen", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(TC_GOLD, "TOURNAMENT"); ImGui::SameLine(); ImGui::Text("%s", t.own_id ? t.own_name : "");
    if (t.err[0]) ImGui::TextColored(TC_RED, "%s", t.err);
    else if (t.pending) ImGui::TextColored(TC_DIM, "Sending...");
    ImGui::Separator();
    if (g_tc_tab) tc_draw_bracket();
    else if (t.set >= 0) tc_draw_set();
    else if (t.own_id && !strcmp(t.own_state, "checkin")) {
        if (!t.own_in) {
            ImGui::SetWindowFontScale(1.4f); ImGui::TextColored(TC_GOLD, "CHECK-IN"); ImGui::SetWindowFontScale(1.0f);
            ImGui::Text("The tournament starts soon. Confirm that you are here to play:");
            ImGui::TextColored(TC_GREEN, "Press A (or Enter) to check in.");
            ImGui::TextColored(TC_DIM, "Who does not check in is left out when the bracket is made.");
            if (g_tc_confirm) ImGui::TextColored(TC_RED, "Leave the tournament? A: yes, leave   B: no");
            tc_hint("A: check in   X: leave the tournament   B: close");
        } else {
            ImGui::TextColored(TC_GREEN, "You are checked in.");
            ImGui::Text("The organizer starts the bracket soon. Keep the game open: your first set appears here by itself.");
            if (g_tc_confirm) ImGui::TextColored(TC_RED, "Leave the tournament? A: yes, leave   B: no");
            tc_hint("A / B: close   X: leave the tournament");
        }
    } else if (t.own_id && !strcmp(t.own_state, "signup")) {
        ImGui::TextColored(TC_GREEN, "You are signed up.");
        ImGui::Text("Before it starts the organizer opens the check-in: this screen opens by itself then, press A to confirm.");
        if (g_tc_confirm) ImGui::TextColored(TC_RED, "Leave the tournament? A: yes, leave   B: no");
        tc_hint("A / B: close   X: leave the tournament");
    } else if (t.own_id && !strcmp(t.own_state, "running")) {
        if (!strcmp(t.status, "out")) {
            ImGui::SetWindowFontScale(1.3f); ImGui::Text("You are out: place %d.", t.place); ImGui::SetWindowFontScale(1.0f);
            ImGui::Text("Thanks for playing! Y shows the bracket.");
        } else if (!strcmp(t.status, "dq")) ImGui::TextColored(TC_RED, "You were disqualified (by the organizer, or you did not come to a set in time).");
        else {
            ImGui::TextColored(TC_GREEN, "Waiting for your next set.");
            if (t.next_br) ImGui::Text("Next: %s round %d vs %s.", t.next_br == 'L' ? "Losers" : t.next_br == 'G' ? "Grand final" : "Winners", t.next_round, t.next_who);
            ImGui::Text("Keep the game open: when the set is ready this screen opens by itself.");
        }
        tc_hint("A / B: close   Y: bracket");
    } else {                                             /* not in a tournament (or the last one is over): the open ones */
        if (t.own_id && !strcmp(t.own_state, "done")) { ImGui::Text("%s is over: you placed %d.", t.own_name, t.place); ImGui::Separator(); }
        int idx[NET_T_LIST], n = tc_open_list(idx);
        if (!n) {
            ImGui::Text("No tournament is open for sign-ups right now.");
            ImGui::TextColored(TC_DIM, "The organizer makes them on the tournament server's web page; a note appears here when one opens.");
            tc_hint("A / B: close");
        } else {
            ImGui::Text("Open tournaments:");
            for (int k = 0; k < n; k++) {
                const NetTourneyItem &it = t.list[idx[k]];
                ImGui::TextColored(k == g_tc_list_sel ? TC_GOLD : ImVec4(1, 1, 1, 1), "%s %s  (%d players)%s", k == g_tc_list_sel ? ">" : " ", it.name, it.players,
                                   it.signed_up ? "  - signed up" : !strcmp(it.state, "checkin") ? "  - check-in open" : "");
            }
            tc_hint("up / down: choose   A: sign up   B: close");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2); ImGui::PopStyleColor();
}
