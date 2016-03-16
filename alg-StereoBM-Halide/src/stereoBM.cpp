#include "Halide.h"
#include "halide_image_io.h"
#include <limits>

using namespace Halide;

int FILTERED = -16;

void apply_default_schedule(Func F) {
    std::map<std::string,Internal::Function> flist = Internal::find_transitive_calls(F.function());
    flist.insert(std::make_pair(F.name(), F.function()));
    std::map<std::string,Internal::Function>::iterator fit;
    for (fit=flist.begin(); fit!=flist.end(); fit++) {
        // Func f(fit->second);
        Internal::Function f = fit->second;
        // Func wrapper(f);
        // wrapper.compute_root();
        if (f.schedule().compute_level().is_inline()){
            std::cout << "Warning: applying inline schedule to " << f.name() << std::endl;
        }
    }
    std::cout << std::endl;
}

Func prefilterXSobel(Func image, int w, int h) {
    Var x("x"), y("y");
    Func clamped("clamped"), gray("gray");
    gray(x, y) = cast<int8_t>(0.2989f*image(x, y, 0) + 0.5870f*image(x, y, 1) + 0.1140f*image(x, y, 2));
    clamped(x, y) = gray(clamp(x, 0, w-1), clamp(y, 0, h-1));

    Func temp("temp"), xSobel("xSobel");
    temp(x, y) = clamped(x+1, y) - clamped(x-1, y);
    xSobel(x, y) = temp(x, y-1) + 2 * clamped(x, y) + clamped(x, y+1);

    // Schedule
    Var xi("xi"), xo("xo"), yi("yi"), yo("yo");
    xSobel.compute_root();
    xSobel.tile(x, y, xo, yo, xi, yi, 128, 32);
    temp.compute_at(xSobel, yi).vectorize(x, 16);
    xSobel.parallel(yo);
    return xSobel;
}

Func findStereoCorrespondence(Func left, Func right, int SADWindowSize, int minDisparity, int numDisparities,
    int width, int height, float uniquenessRatio = 0.15, int disp12MaxDiff = 1) {

    Var x("x"), y("y"), c("c"), d("d");

    Func diff("diff");
    diff(d, x, y) = cast<int>(abs(left(x, y) - right(x-d, y)));

    int win2 = SADWindowSize/2;
    int minD = minDisparity, maxD = minDisparity + numDisparities - 1;
    int xmin = maxD + win2;
    int xmax = width - minD - win2;
    int ymin = win2;
    int ymax = height - win2;

    int x_tile_size = 32, y_tile_size = 32;

    Func diff_T("diff_T");

    Var xi("xi"), xo("xo"), yi("yi"), yo("yo");
    diff_T(d, xi, yi, xo, yo) = diff(d, xi+xo*x_tile_size, yi+yo*y_tile_size);

    Func cSAD("cSAD");
    cSAD(d, xi, yi, xo, yo) = {0, 0};
    RDom r(-win2, x_tile_size + win2, -win2, y_tile_size + win2, "r");
    RVar rxi = r.x, ryi = r.y;
    Expr new_hsum = cSAD(d, rxi-1, ryi, xo, yo)[0] + diff_T(d, rxi+win2, ryi+win2, xo, yo) - select(rxi <= win2, 0, diff_T(d, rxi-win2-1, ryi+win2, xo, yo));
    Expr new_sum = cSAD(d, rxi, ryi-1, xo, yo)[1] + new_hsum - select(ryi <= win2, 0, cSAD(d, rxi, ryi-SADWindowSize, xo, yo)[0]);
    cSAD(d, rxi, ryi, xo, yo) = {new_hsum, new_sum};

    // RDom rd(minDisparity, numDisparities);
    // Expr arg_min = select( cSAD(rd, x, y)[1] < cSAD(1, x, y)[2],
    //                        select(d==0, rd, cSAD(rd, x, y)[1]),
    //                        cSAD(d, x, y)[2]);
    // cSAD(d, x, y) = {cSAD(d, x, y)[0], cSAD(d, x, y)[1], arg_min};

    RDom rd(minDisparity, numDisparities);
    Func disp_left("disp_left");
    disp_left(xi, yi, xo, yo) = {minDisparity, INT_MAX};
    disp_left(xi, yi, xo, yo) = tuple_select(
                            cSAD(rd, xi, yi, xo, yo)[1] < disp_left(xi, yi, xo, yo)[1],
                            {rd, cSAD(rd, xi, yi, xo, yo)[1]},
                            disp_left(xi, yi, xo, yo));
    // check unique match
    // Func unique("unique");
    // unique(x, y) = 1;
    // unique(x, y) = select(rd != disp_left(x, y)[0] && cSAD(rd, x, y)[1] <= disp_left(x, y)[1] * (1 + uniquenessRatio), 0, 1);

    // validate disparity by comparing left and right
    // calculate disp2
    // Func disp_right("disp_right");
    // RDom rx1 = RDom(xmin, xmax - xmin + 1);
    // disp_right(x, y) = {minDisparity, INT_MAX};
    // Expr x2 = clamp(rx1 - disp_left(rx1, y)[0], xmin, xmax);
    // disp_right(x2, y) = tuple_select(
    //                          disp_right(x2, y)[1] > disp_left(rx1, y)[1],
    //                          disp_left(rx1, y),
    //                          disp_right(x2, y)
    //                      );

    Func disp("disp");
    // Expr x2 = clamp(x - disp_left(xi, yi)[0], xmin, xmax);
    // disp(x, y) = select(
    //                 unique(x, y) == 0
    //                 || !(x >= xmin && x <= xmax && y >= ymin && y<= ymax)
    //                 || (x2 >= xmin && x2 <= xmax && abs(disp_right(x2, y)[0] - disp_left(x, y)[0]) > disp12MaxDiff),
    //                 FILTERED,
    //                 disp_left(x,y)[0]
    //              );

    disp(x, y) = select(
                    !(x >= xmin && x <= xmax && y >= ymin && y<= ymax),
                    FILTERED,
                    disp_left(x%x_tile_size, y%y_tile_size, x/x_tile_size, y/y_tile_size)[0]
                 );

    // Schedule
    disp.compute_root();
    disp.tile(x, y, xo, yo, xi, yi, x_tile_size, y_tile_size);
    // unique.compute_inline();
    // disp_right.compute_at(disp, y).vectorize(x, 16);
    disp_left.compute_root().vectorize(xi, 16).parallel(yo);
    disp_left.update().parallel(yo);

    cSAD.compute_at(disp_left, xo).vectorize(d, 16);
    cSAD.update().reorder(d, rxi, ryi, xo, yo).vectorize(d, 16);
    return disp;
}


Func stereoBM(Image<int8_t> left_image, Image<int8_t> right_image, int SADWindowSize, int minDisparity, int numDisparities) {
    Var x("x"), y("y"), c("c");
    Func left("left"), right("right");
    left(x, y, c) = left_image(x, y, c);
    right(x, y, c) = right_image(x, y, c);

    int width = left_image.width();
    int height = left_image.height();

    Func filteredLeft = prefilterXSobel(left, width, height);
    Func filteredRight = prefilterXSobel(right, width, height);

    /* get valid disparity region */
    Func disp = findStereoCorrespondence(filteredLeft, filteredRight, SADWindowSize, minDisparity, numDisparities,
        left_image.width(), left_image.height());

    disp.compute_root();
    apply_default_schedule(disp);

    disp.compile_to_lowered_stmt("disp.html", {}, HTML);
    // Image<int> filtered = filteredLeft.realize(width, height, 3);
    // Halide::Tools::save_image(filtered, "filteredLeft.png");
    return disp;
}
