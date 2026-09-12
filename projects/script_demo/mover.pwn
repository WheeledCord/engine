/* The authoring demo's Mover again, in Pawn. Every call comes from the engine's binding table, the
   same one the Scheme version uses; engine.inc is written from that table by the build. */

#include <engine>

forward mover_spawn();
forward mover_think();
forward mover_draw();

public mover_spawn()
{
    think_next();
}

public mover_think()
{
    new Float:speed = get("speed");
    new Float:per_step = speed * dt();
    new Float:ix, Float:iy;
    input_vector(ix, iy);
    move_world(ix * per_step, iy * per_step);

    new turn = key_down(key("e")) - key_down(key("q"));
    rotate(float(turn) * 2.0 * dt());
    if (key_down(key("space")))
        move_local(per_step, 0.0);
    think_next();
}

public mover_draw()
{
    new body = rgba(102, 191, 255, 255);
    new arrow = rgba(0, 82, 172, 255);
    new Float:px, Float:py;
    interpolated(px, py);
    draw_rect_rotated(px, py, 32.0, 32.0, interpolated_rotation(), body);

    // The arrow points along local +X, so Space follows it whichever way the square faces.
    new Float:ax, Float:ay, Float:bx, Float:by, Float:cx, Float:cy;
    to_local(12.0, 0.0, ax, ay);
    to_local(-6.0, -8.0, bx, by);
    to_local(-6.0, 8.0, cx, cy);
    draw_triangle(ax, ay, bx, by, cx, cy, arrow);
}

main()
{
    class_new("mover");
    class_field("mover", "speed", "float");
    class_on("mover", "spawn", "mover_spawn");
    class_on("mover", "think", "mover_think");
    class_on("mover", "draw", "mover_draw");
    class_done("mover");
}
