// A real recursive ray tracer: spheres, lambertian + metal materials,
// antialiasing, gamma correction. Writes a binary PPM and prints a checksum so
// the run is self-verifying. Deterministic (fixed LCG seed) => reproducible.
const std = @import("std");

const Vec3 = struct {
    x: f64,
    y: f64,
    z: f64,
    fn add(a: Vec3, b: Vec3) Vec3 {
        return .{ .x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z };
    }
    fn sub(a: Vec3, b: Vec3) Vec3 {
        return .{ .x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z };
    }
    fn scale(a: Vec3, t: f64) Vec3 {
        return .{ .x = a.x * t, .y = a.y * t, .z = a.z * t };
    }
    fn mul(a: Vec3, b: Vec3) Vec3 {
        return .{ .x = a.x * b.x, .y = a.y * b.y, .z = a.z * b.z };
    }
    fn dot(a: Vec3, b: Vec3) f64 {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    fn len(a: Vec3) f64 {
        return @sqrt(a.dot(a));
    }
    fn unit(a: Vec3) Vec3 {
        return a.scale(1.0 / a.len());
    }
    fn reflect(v: Vec3, n: Vec3) Vec3 {
        return v.sub(n.scale(2 * v.dot(n)));
    }
};

// Deterministic LCG so the image (and its checksum) is identical every run.
var rng_state: u64 = 0x2545F4914F6CDD1D;
fn rand() f64 {
    rng_state = rng_state *% 6364136223846793005 +% 1442695040888963407;
    const bits: u64 = (rng_state >> 11) | 0x3FF0000000000000;
    return @as(f64, @bitCast(bits)) - 1.0;
}
fn randInUnitSphere() Vec3 {
    while (true) {
        const p = Vec3{ .x = 2 * rand() - 1, .y = 2 * rand() - 1, .z = 2 * rand() - 1 };
        if (p.dot(p) < 1.0) return p;
    }
}

const Material = struct { kind: enum { lambertian, metal }, albedo: Vec3, fuzz: f64 = 0 };
const Sphere = struct { center: Vec3, radius: f64, mat: Material };
const Ray = struct { origin: Vec3, dir: Vec3 };
const Hit = struct { t: f64, p: Vec3, n: Vec3, mat: Material };

fn hitSphere(s: Sphere, r: Ray, tmin: f64, tmax: f64) ?Hit {
    const oc = r.origin.sub(s.center);
    const a = r.dir.dot(r.dir);
    const half_b = oc.dot(r.dir);
    const c = oc.dot(oc) - s.radius * s.radius;
    const disc = half_b * half_b - a * c;
    if (disc < 0) return null;
    const sd = @sqrt(disc);
    var root = (-half_b - sd) / a;
    if (root < tmin or root > tmax) {
        root = (-half_b + sd) / a;
        if (root < tmin or root > tmax) return null;
    }
    const p = r.origin.add(r.dir.scale(root));
    return .{ .t = root, .p = p, .n = p.sub(s.center).scale(1.0 / s.radius), .mat = s.mat };
}

fn hitWorld(world: []const Sphere, r: Ray) ?Hit {
    var best: ?Hit = null;
    var closest: f64 = 1e30;
    for (world) |s| {
        if (hitSphere(s, r, 0.001, closest)) |h| {
            closest = h.t;
            best = h;
        }
    }
    return best;
}

fn color(r: Ray, world: []const Sphere, depth: u32) Vec3 {
    if (depth == 0) return .{ .x = 0, .y = 0, .z = 0 };
    if (hitWorld(world, r)) |h| {
        switch (h.mat.kind) {
            .lambertian => {
                const target = h.p.add(h.n).add(randInUnitSphere());
                const scattered = Ray{ .origin = h.p, .dir = target.sub(h.p) };
                return h.mat.albedo.mul(color(scattered, world, depth - 1));
            },
            .metal => {
                const reflected = r.dir.unit().reflect(h.n);
                const scattered = Ray{ .origin = h.p, .dir = reflected.add(randInUnitSphere().scale(h.mat.fuzz)) };
                if (scattered.dir.dot(h.n) <= 0) return .{ .x = 0, .y = 0, .z = 0 };
                return h.mat.albedo.mul(color(scattered, world, depth - 1));
            },
        }
    }
    const t = 0.5 * (r.dir.unit().y + 1.0);
    const white = Vec3{ .x = 1, .y = 1, .z = 1 };
    const blue = Vec3{ .x = 0.5, .y = 0.7, .z = 1.0 };
    return white.scale(1 - t).add(blue.scale(t));
}

pub fn main() !void {
    const nx: usize = 320;
    const ny: usize = 180;
    const ns: usize = 24; // samples per pixel

    const world = [_]Sphere{
        .{ .center = .{ .x = 0, .y = -100.5, .z = -1 }, .radius = 100, .mat = .{ .kind = .lambertian, .albedo = .{ .x = 0.8, .y = 0.8, .z = 0.0 } } },
        .{ .center = .{ .x = 0, .y = 0, .z = -1 }, .radius = 0.5, .mat = .{ .kind = .lambertian, .albedo = .{ .x = 0.7, .y = 0.3, .z = 0.3 } } },
        .{ .center = .{ .x = 1, .y = 0, .z = -1 }, .radius = 0.5, .mat = .{ .kind = .metal, .albedo = .{ .x = 0.8, .y = 0.6, .z = 0.2 }, .fuzz = 0.05 } },
        .{ .center = .{ .x = -1, .y = 0, .z = -1 }, .radius = 0.5, .mat = .{ .kind = .metal, .albedo = .{ .x = 0.8, .y = 0.8, .z = 0.8 }, .fuzz = 0.2 } },
    };

    const origin = Vec3{ .x = 0, .y = 0, .z = 0 };
    const lower_left = Vec3{ .x = -2, .y = -1.125, .z = -1 };
    const horizontal = Vec3{ .x = 4, .y = 0, .z = 0 };
    const vertical = Vec3{ .x = 0, .y = 2.25, .z = 0 };

    var file = try std.fs.cwd().createFile("out.ppm", .{});
    defer file.close();
    var bw = std.io.bufferedWriter(file.writer());
    const w = bw.writer();
    try w.print("P6\n{d} {d}\n255\n", .{ nx, ny });

    var checksum: u64 = 1469598103934665603; // FNV-1a
    var j: usize = ny;
    while (j > 0) {
        j -= 1;
        var i: usize = 0;
        while (i < nx) : (i += 1) {
            var col = Vec3{ .x = 0, .y = 0, .z = 0 };
            var s: usize = 0;
            while (s < ns) : (s += 1) {
                const u = (@as(f64, @floatFromInt(i)) + rand()) / @as(f64, @floatFromInt(nx));
                const v = (@as(f64, @floatFromInt(j)) + rand()) / @as(f64, @floatFromInt(ny));
                const dir = lower_left.add(horizontal.scale(u)).add(vertical.scale(v)).sub(origin);
                col = col.add(color(.{ .origin = origin, .dir = dir }, &world, 24));
            }
            col = col.scale(1.0 / @as(f64, @floatFromInt(ns)));
            const rgb = [3]u8{
                @intFromFloat(255.99 * std.math.clamp(@sqrt(col.x), 0, 0.999)),
                @intFromFloat(255.99 * std.math.clamp(@sqrt(col.y), 0, 0.999)),
                @intFromFloat(255.99 * std.math.clamp(@sqrt(col.z), 0, 0.999)),
            };
            try w.writeAll(&rgb);
            for (rgb) |b| {
                checksum = (checksum ^ b) *% 1099511628211;
            }
        }
    }
    try bw.flush();

    const so = std.io.getStdOut().writer();
    try so.print("raytrace: {d}x{d}, {d} spp -> out.ppm  checksum=0x{x}\n", .{ nx, ny, ns, checksum });
}
