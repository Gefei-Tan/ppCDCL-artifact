#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include <iostream>

using namespace std;
using namespace emp;
using Bit = Bit_T<AuthWire>;
using Integer = Integer_T<AuthWire>;

const int depth = 20;

MPC<RandomIO> * base;

void init_backend(int party) {
    const int threads = 2;
    RandomIO ** io = new RandomIO *[2];
    for (int i = 0; i < threads; i++) {
        io[i] = new RandomIO((party-1) ? "127.0.0.1" : nullptr, 12345+i, true);
    }
    block delta; PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE) delta ^= makeBlock(0, 1);
    else delta ^= makeBlock(0, 3);
    base = new MPC<RandomIO>(party, threads, io, delta);
    emp::backend = base;
}

void test_check(int run = 1000, int length = 10) {
    for (int T = 0; T < run; T++) {
        bool r[length]; block R[length], K[length], M[length];
        PRG().random_bool(r, length);
        if (base->party == ALICE) {
            base->ot_send->send_cot(K, length);
            base->ot_recv->recv_cot(M, r, length);
        } else {
            base->ot_recv->recv_cot(M, r, length);
            base->ot_send->send_cot(K, length);
        }
        for (int i = 0; i < length; i++) {
            M[i] = K[i] ^ M[i] ^ (r[i] ? base->delta : zero_block);
            R[i] = r[i] ? makeBlock(0, 1) : zero_block;
        }
        base->check_buffer(R, M, length);
    }
    cout << base->check_digest() << endl;
}

void test_bit() {
    bool b[] = {true, false};
    int p[] = {PUBLIC, ALICE, BOB};
    for(int i = 0; i < 2; ++i)
        for(int j = 0; j < 3; ++j)
            for(int k = 0; k < 2; ++k)
                for (int l= 0; l < 3; ++l)  {
                        Bit b1(b[i], p[j]); Bit b2(b[k], p[l]); Bit b3;
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 & b2;}, b1, b2, b3);
                        bool res = b3.reveal(PUBLIC);
                        if(res != (b[i] and b[k])) {
                            cout<<"AND" <<i<<" "<<j<<" "<<k<<" "<<l<<" "<<res<<endl;
                            error("test AND error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 & b1;}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (res != b[i]) {
                            cout<<"AND" <<i<<" "<<j<<res<<endl;
                            error("test AND error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 & (!b1);}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (res) {
                            cout<<"AND" <<i<<" "<<j<<res<<endl;
                            error("test AND error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 ^ b2;}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if(res != (b[i] xor b[k])) {
                            cout <<"XOR"<<i<<" "<<j<<" "<<k<<" "<<l<< " " <<res<<endl;
                            error("test XOR error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 ^ b1;}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (res) {
                            cout<<"XOR" <<i<<" "<<j<<res<<endl;
                            error("test XOR error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 ^ (!b1);}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (!res) {
                            cout<<"XOR" <<i<<" "<<j<<res<<endl;
                            error("test XOR error!");
                        }
                    }
    std::cout << "test bit success!" << std::endl;
}

template<typename Op, typename Op2>
void test_int(int party, int range1 = 1<<25, int range2 = 1<<25, bool check = true, int runs = 10) {
    block seed = makeBlock(0,1);
    PRG prg( &seed );
    for(int i = 0; i < runs; ++i) {
        long long ia, ib;
        prg.random_data(&ia, 8);
        prg.random_data(&ib, 8);
        ia %= range1;
        ib %= range2;
        while( Op()(int(ia), int(ib)) != Op()(ia, ib) ) {
            prg.random_data(&ia, 8);
            prg.random_data(&ib, 8);
            ia %= range1;
            ib %= range2;
        }	

        Integer a(32, ia, ALICE);
        Integer b(32, ib, BOB);

        Integer res;

        if (!check) {
            base->execute(
                [](Integer& a, Integer& b, Integer& res, int runs) { for(int i = 0; i < runs; ++i) res = Op2()(a,b); }, 
                a, b, res, runs
            );
            break;
        } else {
            base->execute(
                [](Integer& a, Integer& b, Integer& res) { res = Op2()(a,b); }, 
                a, b, res
            );
            if (res.reveal<int32_t>(PUBLIC) != Op()(ia,ib)) {
                std::cout << ia << " " << ib << std::endl;
                cout << a.reveal<int32_t>()<<endl;
                cout << b.reveal<int32_t>()<<endl;
                cout << ia <<"\t"<<ib<<"\t"<<Op()(ia,ib)<<"\t"<<res.reveal<int32_t>(PUBLIC)<<endl;
            }
            assert(res.reveal<int32_t>(PUBLIC) == Op()(ia,ib));
        }
    }
    cout << typeid(Op2).name()<<"\t\t\tDONE"<<endl;
}

void test_stash(int run = 1000, int sz = 1000) {
    block seed = makeBlock(0,1);
    PRG *prg = new PRG( &seed );

    Stash *stash = new Stash(sz, 32);
    int8_t** data = new int8_t*[sz];
    for (int i = 0; i < sz; i++) {
        data[i] = new int8_t[16];
        prg->random_data(data[i], 16);
    }

    base->execute(
        [&stash, data, sz]() {
            for (int i = 0; i < sz; i++) {
                Integer idx(32, i, PUBLIC);
                stash->write(idx, Integer(128, data[i], PUBLIC));
            }
        }
    );

    Integer val(128, 0, PUBLIC);
    for (int t = 0; t < run; t++) {
        Bit found(false, PUBLIC);
        Integer idx(32, t % sz, BOB);
        
        base->execute(
            [&stash, idx, &found, &val]() {stash->read(idx, found, val, true);}
        );
        for (int i = 0; i < 128; i++) {
            bool res = (bool) ((((uint8_t *) data[t % sz])[i / 8] & (((uint8_t) 1) << (i % 8))) != 0);
            if (val.bits[i].reveal(PUBLIC) != res) {
                cout << "error " << t << " " << i << endl;
                cout << val.bits[i].reveal(PUBLIC) << " " << res << endl;
                error("stash error!");
            }
        }
        prg->random_data(data[t % sz], 16);
        base->execute(
            [&stash, idx, data, t, sz]() {
                stash->write(idx, Integer(128, data[t % sz], PUBLIC));
            }
        );
    }
}

void test_dpf(int sz=5, int a=3, int b=2) {
    std::cout << "DPF testing results:" << std::endl;
    Integer alpha(sz, a, PUBLIC);
    Integer beta(128, b, PUBLIC);
    block *r, beta_value = makeBlock(0, b);
    base->DPF(alpha, beta, 5, &r);
    base->check_digest();
    base->direct_open(r, 1<<(sz+1));
    for (int i = 0; i < 1<<sz; i++) {
        if (i != a && !cmpBlock(&r[i<<1], &zero_block, 1)) {
            cout << "r[" <<  i << "]: " << r[i<<1] << endl;
            error("DPF error at zero!");
        } if (i == a && !cmpBlock(&r[i<<1], &beta_value, 1)) {
            error("DPF error at alpha/beta");
        } else if (i == a) {
            cout << "r[" <<  i << "]: " << r[i<<1] << endl;
        }
    }
}

void test_aes() {
    block k = makeBlock(1,0); int index = 10; 
    Integer message(depth, index, PUBLIC);
    Integer key(&k, PUBLIC);

    block res[2];
    base->AES(message, key, res);
    base->direct_open(res, 2);
    std::cout << "AES testing results:" << std::endl;
    std::cout << res[0] << " " << res[1] << std::endl;

    AES_KEY aes_key[2];
    res[0] = res[1] = makeBlock(0, index);
    AES_set_encrypt_key((const block) k, &aes_key[0]);
    AES_set_encrypt_key((const block) k^makeBlock(0,1), &aes_key[1]);
    ParaEnc<2,1>(res, aes_key);
    std::cout << res[0] << " " << res[1] << std::endl;
}

void output(RAM<RandomIO> *arr, string name, int sz) {
    for (int i = 0; i < sz; i++) {
        Integer value;
        arr->read(i, value);
        cout << name << "[" << i << "]: ";
        cout << value.reveal<int64_t>(PUBLIC) << endl;
    }
}

void test_oram(int sz = 10) {
    std::cout << "ORAM testing results:" << std::endl;
    int bit = 3, bit_z = 6;
    cout << "set x[i]=i, y[i]=2i, z[0]=z[1]=1" << endl;
    Integer *arr_x = new Integer[sz], *arr_y = new Integer[sz], *arr_z = new Integer[sz];
    for (int i = 0; i < sz; i++) {
        arr_x[i] = Integer(1<<bit, i, PUBLIC);
        arr_y[i] = Integer(1<<bit, i<<1, PUBLIC);
        arr_z[i] = Integer(1<<bit_z, 1, PUBLIC);
    }
    RAM<RandomIO> *x, *y, *z;
    x = new RAM<RandomIO>(base, depth, bit, -1, arr_x, 0, sz);
    y = new RAM<RandomIO>(base, depth, bit, -1, arr_y, 0, sz);
    z = new RAM<RandomIO>(base, depth, bit_z, -1, arr_z, 0, 2);

    cout << "x[i]=3*x[i]" << endl;
    for (int i = 0; i < sz; i++) {
        Integer idx(depth, i, PUBLIC);
        x->write(idx, 
            [&](const Integer &in, Integer &out) {out = (in<<1) + in;}
        );
    }
    output(x, "x", sz);

    cout << "y[i]=x[i]-y[i]" << endl;
    for (int i = 0; i < sz; i++) {
        Integer idx(depth, i, PUBLIC); 
        Integer x_i;
        x->read(idx, x_i);
        y->write(idx, 
            [&](const Integer &in, Integer &out) {out = x_i - in;}
        );
    }
    y->refresh(y->ka, y->kb);
    output(y, "y", sz);
    
    cout << "z[i]=z[i-1]+z[i-2]" << endl;
    for (int i = 2; i < sz; i++) {
        Integer z1, z2;
        z->read(i-1, z1);
        z->read(i-2, z2);
        z->write(i, [&](const Integer &in, Integer &out) {out = z1+z2;});
    }
    z->refresh(z->ka, z->kb);
    output(z, "z", sz);
    
    cout << "consistency: " << base->check_digest() << endl;
}


int main(int argc, char** argv) {
    int party = atoi(argv[1]);
    init_backend(party);

    auto start = clock_start();

    test_bit();

    test_int<std::plus<int>, std::plus<Integer>>(party);
    test_int<std::minus<int>, std::minus<Integer>>(party);
    test_int<std::multiplies<int>, std::multiplies<Integer>>(party);
    test_int<std::divides<int>, std::divides<Integer>>(party);
    test_int<std::modulus<int>, std::modulus<Integer>>(party);

    test_int<std::bit_and<int>, std::bit_and<Integer>>(party);
    test_int<std::bit_or<int>, std::bit_or<Integer>>(party);
    test_int<std::bit_xor<int>, std::bit_xor<Integer>>(party);

    test_aes();
    test_dpf();
    test_oram();

    long long t = time_from(start);
    std::cout << "gates:" << emp::backend->num_and() << "\n"; 
    std::cout << emp::backend->num_and() * 1e6 / t << "\n"; 
    delete emp::backend;
    return 0;
}
