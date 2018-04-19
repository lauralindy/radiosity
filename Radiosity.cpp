#include "Radiosity.h"
#include <glm/gtx/intersect.hpp>
#include <optixu/optixu_math_namespace.h>
#include <optixu/optixpp_namespace.h>
// helper functions and utilities to work with CUDA

#include <math.h>
#include <sutil.h>

//save me
#define INITIAL_LIGHT_EMITTER_INTENSITY		10.0f
#define INITIAL_AMBIENT_INTENSITY			glm::vec3(0.00f, 0.00f, 0.00f)

#define RADIOSITY_SOLUTION_THRESHOLD		glm::vec3(0.25f, 0.25f, 0.25f)
#define FORM_FACTOR_SAMPLES					750
optix::Context context = 0;
optix::Buffer vertices, faces, normals;
optix::Buffer outputFaces, distances;
optix::Buffer rays;
optix::Program boundingProgram, intersectionProgram, diffuse_ch;


const char* const SAMPLE_NAME = "../../../../Users/PCG DEMO/Desktop/CustomRadiosity - Copy";

void destroyContext()
{
	if (context)
	{
		context->destroy();
		context = 0;
	}
}


glm::vec2 Radiosity::getTotalCounts(Mesh *mesh) {

	int totalFaces, totalVertices;

	for (int i = 0; i < mesh->sceneModel.size(); i++)
	{
		//for every scene object
		ObjectModel* currentObject = &mesh->sceneModel[i].obj_model;

		int currentObjFaces = currentObject->faces.size();
		totalFaces += currentObjFaces;
		for (int j = 0; j < currentObjFaces; j++) {
			ModelFace face = currentObject->faces[j];
			totalVertices += face.vertexIndexes.size();

		}

	}
	return glm::vec2(totalFaces, totalVertices);
}

void Radiosity::loadSceneFacesFromMesh(Mesh* mesh)
{
	destroyContext(); // resets the context before passing new values
	context = optix::Context::create();
	context->setRayTypeCount(2);

	const char *ptx = sutil::getPtxString(SAMPLE_NAME, "RayTrace.cu");
	context->setRayGenerationProgram(0, context->createProgramFromPTXString(ptx, "pathtrace_camera"));
	context->setExceptionProgram(0, context->createProgramFromPTXString(ptx, "exception"));
	context->setMissProgram(0, context->createProgramFromPTXString(ptx, "miss"));

	//diffuse_ch = context->createProgramFromPTXString(ptx, "diffuse");
	//diffuse_ch->setClosestHitProgram(0, diffuse_ch);
	ptx = sutil::getPtxString(SAMPLE_NAME, "parallelogram.cu");
	boundingProgram = context->createProgramFromPTXString(ptx, "bounds");
	intersectionProgram = context->createProgramFromPTXString(ptx, "intersect");


	glm::vec2 totals = getTotalCounts(mesh);

	vertices = context->createBuffer(RT_BUFFER_INPUT, RT_FORMAT_FLOAT3, totals[1]);
	normals = context->createBuffer(RT_BUFFER_INPUT, RT_FORMAT_FLOAT3, totals[0]);
	faces = context->createBuffer(RT_BUFFER_INPUT, RT_FORMAT_UNSIGNED_INT4, totals[0]);
	outputFaces = sutil::createInputOutputBuffer(context, RT_FORMAT_FLOAT, FORM_FACTOR_SAMPLES, 0, true);
	distances = sutil::createInputOutputBuffer(context, RT_FORMAT_FLOAT, FORM_FACTOR_SAMPLES, 0, true);

	optix::float3 *vertexMap = reinterpret_cast<optix::float3*>(vertices->map());
	optix::float3 *normalMap = reinterpret_cast<optix::float3*>(normals->map());
	optix::uint4 *faceMap = reinterpret_cast<optix::uint4*>(faces->map());

	sceneFaces.clear();
	formFactors.clear();

	int vertexCtr, faceCtr;

	for (int i = 0; i<mesh->sceneModel.size(); i++)
	{
		//for every scene object
		ObjectModel* currentObject = &mesh->sceneModel[i].obj_model;

		int currentObjFaces = currentObject->faces.size();

		for (int j = 0; j < currentObjFaces; j++)
		{
			//for every face of it
			ModelFace* currentFace = &mesh->sceneModel[i].obj_model.faces[j];

			RadiosityFace radiosityFace;

			radiosityFace.model = currentObject;
			radiosityFace.faceIndex = j;
			if (currentFace->material->illuminationMode != 1)
				radiosityFace.emission = currentFace->material->diffuseColor * INITIAL_LIGHT_EMITTER_INTENSITY;
			else
				radiosityFace.emission = currentFace->material->diffuseColor * INITIAL_AMBIENT_INTENSITY;

			radiosityFace.totalRadiosity = radiosityFace.emission;
			radiosityFace.unshotRadiosity = radiosityFace.emission;

			sceneFaces.push_back(radiosityFace);

			//writing into buffers for Optix
			std::vector<GLuint> f = currentFace->vertexIndexes;
			faceMap[faceCtr] = optix::make_uint4(f[0], f[1], f[2], f[3]);
			glm::vec3 n = currentObject->getFaceNormal(sceneFaces[i].faceIndex);
			normalMap[faceCtr++] = optix::make_float3(n[0], n[1], n[2]);

			int v0_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[0];
			int v1_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[1];
			int v2_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[2];
			int v3_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[3];

			glm::vec3 A = sceneFaces[i].model->vertices[v0_k_index];
			glm::vec3 B = sceneFaces[i].model->vertices[v1_k_index];
			glm::vec3 C = sceneFaces[i].model->vertices[v2_k_index];
			glm::vec3 D = sceneFaces[i].model->vertices[v3_k_index];
			vertexMap[vertexCtr++] = optix::make_float3(A[0], A[1], A[2]);
			vertexMap[vertexCtr++] = optix::make_float3(B[0], B[1], B[2]);
			vertexMap[vertexCtr++] = optix::make_float3(C[0], C[1], C[2]);
			vertexMap[vertexCtr++] = optix::make_float3(D[0], D[1], D[2]);
		}
	}

	context["vertex_buffer"]->setBuffer(vertices);
	context["normal_buffer"]->setBuffer(normals);
	context["face_buffer"]->setBuffer(faces);
	context["outputFaces"]->set(outputFaces);
	context["distances"]->set(distances);


	optix::Geometry geometry = context->createGeometry();
	geometry->setPrimitiveCount(totals[0]);
	geometry->setIntersectionProgram(intersectionProgram);
	geometry->setBoundingBoxProgram(boundingProgram);
	geometry->setPrimitiveIndexOffset(0);

	optix::GeometryInstance gi = context->createGeometryInstance();
	gi->setGeometry(geometry);
	std::vector<optix::GeometryInstance> gis;
	gis.push_back(gi);
	optix::GeometryGroup geo = context->createGeometryGroup(gis.begin(), gis.end());
	geo->setAcceleration(context->createAcceleration("Trbvh"));
	context["top_object"]->set(geo);

	formFactors.resize(sceneFaces.size(), vector<double>(sceneFaces.size()));
}

int Radiosity::getMaxUnshotRadiosityFaceIndex()
{
	int index = -1;
	double maxUnshot = 0.0;

	for (int i = 0; i<sceneFaces.size(); i++)
	{
		double curUnshot = glm::length(sceneFaces[i].unshotRadiosity);
		double curArea = sceneFaces[i].model->getFaceArea(sceneFaces[i].faceIndex);

		curUnshot *= curArea;

		if (curUnshot > maxUnshot)
		{
			index = i;
			maxUnshot = curUnshot;
		}
	}
	return index;

}


glm::vec3 getCosineDistributionVector(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 norm) {
	float sin_theta = sqrt((double)rand() / ((double)RAND_MAX + 1));
	float cos_theta = sqrt(1 - sin_theta * sin_theta);

	float psi = ((double)rand() / ((double)RAND_MAX + 1) * 2 * 3.14159265359);

	glm::vec3 a1 = glm::vec3(sin_theta) * cos(psi);
	glm::vec3 b1 = glm::vec3(sin_theta) * sin(psi);
	glm::vec3 c1 = glm::vec3(cos_theta);


	glm::vec3 v1 = a1 * (a - b);
	glm::vec3 v2 = b1 * (a - c);
	glm::vec3 v3 = c1 * norm;

	return v1 + v2 + v3;
}


optix::Buffer getOutputBuffer()
{
	return context["output_buffer"]->getBuffer();
}

void Radiosity::calculateFormFactorsForFace(int i, int samplePointsCount)
{
	vector<Ray> generated_dir(samplePointsCount);

	vector<glm::vec3> samplePoints_i = sceneFaces[i].model->monteCarloSamplePoints(sceneFaces[i].faceIndex, samplePointsCount);

	glm::vec3 normal_i = sceneFaces[i].model->getFaceNormal(sceneFaces[i].faceIndex);


	int v0_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[0];
	int v1_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[1];
	int v2_k_index = sceneFaces[i].model->faces[sceneFaces[i].faceIndex].vertexIndexes[2];

	glm::vec3 A = sceneFaces[i].model->vertices[v0_k_index];
	glm::vec3 B = sceneFaces[i].model->vertices[v1_k_index];
	glm::vec3 C = sceneFaces[i].model->vertices[v2_k_index];

	// initializes the rays buffer
	rays = context->createBuffer(RT_BUFFER_INPUT, RT_FORMAT_USER, samplePointsCount);

	optix::Ray *rayMap = reinterpret_cast<optix::Ray*>(rays->map());
	// type of ray we're creating 
	context->setRayTypeCount(2);

	//We generate a ray and direction based on the various different locations on the face
	for (int j = 0; j < samplePointsCount; j++) {

		glm::vec3 direction = glm::normalize(getCosineDistributionVector(A, B, C, normal_i));

		generated_dir[j] = Ray(samplePoints_i[j], (direction));
		optix::float3 origin = optix::make_float3(samplePoints_i[j].x, samplePoints_i[j].y, samplePoints_i[j].z);
		optix::float3 dir = optix::make_float3(direction.x, direction.y, direction.z);

		//creates the iterable RT Rays
		rayMap[j] = optix::make_Ray(origin, dir, 2, 0.001f, 1000000.0f);

	}

	context["ray"]->setBuffer(rays);
	for (int j = 0; j < samplePointsCount; j++) {
		int k;
		float  distance;
		glm::vec3 HitPoint;
		if (isVisibleFrom(generated_dir[j], k, distance, HitPoint)) {
			//printf("The hitpoint is %f %f %f \n", HitPoint[0], HitPoint[1], HitPoint[2]);
			glm::vec3 normal_j = sceneFaces[k].model->getFaceNormal(sceneFaces[k].faceIndex);
			float area_k = sceneFaces[k].model->getFaceArea(sceneFaces[k].faceIndex);

			glm::vec3 r_ij = glm::normalize(HitPoint - samplePoints_i[j]);
			float r_squared = distance * distance;

			double cos_angle_i = glm::dot(r_ij, normal_i);
			double cos_angle_j = glm::dot(r_ij, normal_j);

			double delta_F = (cos_angle_i * cos_angle_j) / (3.14159265359 * r_squared + (area_k / samplePointsCount));

			if (abs(delta_F) > 0.0)
				formFactors[i][k] = formFactors[i][k] + abs(delta_F);

		}

	}

}

void Radiosity::PrepareUnshotRadiosityValues()
{
	for (int i = 0; i<sceneFaces.size(); i++)
	{
		sceneFaces[i].totalRadiosity = sceneFaces[i].emission;
		sceneFaces[i].unshotRadiosity = sceneFaces[i].emission;
	}
}

void Radiosity::calculateRadiosityValues()
{
	glm::vec3 threshold = RADIOSITY_SOLUTION_THRESHOLD;
	glm::vec3 error(100.0f, 100.0f, 100.0f);

	/*
	for(int i=0; i<sceneFaces.size(); i++)
	{
	sceneFaces[i].totalRadiosity = sceneFaces[i].emission;
	sceneFaces[i].unshotRadiosity = sceneFaces[i].emission;
	}
	*/
	int iterations = 0;
	while (
		error.r > threshold.r &&
		error.g > threshold.g &&
		error.b > threshold.b && iterations < 100
		)
	{
		int i = getMaxUnshotRadiosityFaceIndex();

		calculateFormFactorsForFace(i, FORM_FACTOR_SAMPLES);

		for (int j = 0; j< sceneFaces.size(); j++)
		{
			glm::dvec3 p_j = (glm::dvec3)sceneFaces[j].model->faces[sceneFaces[j].faceIndex].material->diffuseColor;

			glm::dvec3 delta_rad = sceneFaces[i].unshotRadiosity * formFactors[i][j] * p_j;

			sceneFaces[j].unshotRadiosity = sceneFaces[j].unshotRadiosity + delta_rad;
			sceneFaces[j].totalRadiosity = sceneFaces[j].totalRadiosity + delta_rad;
		}

		sceneFaces[i].unshotRadiosity = glm::dvec3(0.0f, 0.0f, 0.0f);

		int e = getMaxUnshotRadiosityFaceIndex();
		error = sceneFaces[e].unshotRadiosity;
		iterations = iterations + 1;
	}

}

void Radiosity::setMeshFaceColors()
{
	for (int i = 0; i< sceneFaces.size(); i++)
	{
		glm::dvec3 radValue = glm::clamp(sceneFaces[i].totalRadiosity, 0.0, 1.0);

		glm::clamp(radValue, 0.0, 1.0);
		sceneFaces[i].model->faces[sceneFaces[i].faceIndex].intensity = radValue;
	}
}

bool Radiosity::isParallelToFace(Ray* r, int i)
{
	glm::vec3 n = sceneFaces[i].model->getFaceNormal(sceneFaces[i].faceIndex);
	float dotProduct = abs(glm::dot(n, r->getDirection()));
	if (dotProduct <= 0.001f)
		return true;
	return false;
}

struct RayHit
{
	float distance;
	int hitSceneFaceIndex;
	glm::vec3 hitpoint;
};

bool rayHit_LessThan(RayHit r1, RayHit r2)
{
	if (r1.distance < r2.distance)
		return true;
	return false;
}

bool Radiosity::isVisibleFrom(int i, int j)
{

	vector<RayHit> rayHits;

	//get both centroids
	glm::vec3 centroid_i = sceneFaces[i].model->getFaceCentroid(sceneFaces[i].faceIndex);
	glm::vec3 centroid_j = sceneFaces[j].model->getFaceCentroid(sceneFaces[j].faceIndex);

	//now make a ray
	Ray ray(centroid_i, centroid_j - centroid_i);

	for (int k = 0; k<sceneFaces.size(); k++)
	{
		if (k == i)
			continue;

		int v0_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[0];
		int v1_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[1];
		int v2_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[2];

		glm::vec3 A = sceneFaces[k].model->vertices[v0_k_index];
		glm::vec3 B = sceneFaces[k].model->vertices[v1_k_index];
		glm::vec3 C = sceneFaces[k].model->vertices[v2_k_index];

		glm::vec3 hitPoint;
		if (glm::intersectRayTriangle(centroid_i, centroid_j - centroid_i, A, B, C, hitPoint))
		{
			RayHit currentHit;
			currentHit.distance = glm::distance(ray.getStart(), hitPoint);
			currentHit.hitSceneFaceIndex = k;
			rayHits.push_back(currentHit);
		}
		/*
		if(doesRayHit(&ray, k, hitPoint))
		{
		RayHit currentHit;
		currentHit.distance = glm::distance(ray.getStart(), hitPoint);
		currentHit.hitSceneFaceIndex = k;
		rayHits.push_back(currentHit);
		}
		*/
	}
	if (rayHits.empty())
		return false;

	if (rayHits.size() == 1)
		return true;

	return false;
}

bool Radiosity::isVisibleFrom(glm::vec3 point_j, glm::vec3 point_i)
{
	vector<RayHit> rayHits;

	//now make a ray
	Ray ray(point_i, point_j - point_i);

	for (int k = 0; k<sceneFaces.size(); k++)
	{
		int v0_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[0];
		int v1_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[1];
		int v2_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[2];

		glm::vec3 A = sceneFaces[k].model->vertices[v0_k_index];
		glm::vec3 B = sceneFaces[k].model->vertices[v1_k_index];
		glm::vec3 C = sceneFaces[k].model->vertices[v2_k_index];

		glm::vec3 hitPoint;
		if (glm::intersectRayTriangle(point_i, point_j - point_i, A, B, C, hitPoint))
		{
			RayHit currentHit;
			currentHit.distance = glm::distance(ray.getStart(), hitPoint);
			currentHit.hitSceneFaceIndex = k;
			rayHits.push_back(currentHit);
		}

	}

	// return's the values if it hits
	return (rayHits.size() == 1);

}

bool Radiosity::isVisibleFrom(Ray input, int & global_k, float & global_distance, glm::vec3  & r_ij)
{
	vector<RayHit> rayHits;
	Ray ray = input;

	for (int k = 0; k<sceneFaces.size(); k++)
	{
		int v0_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[0];
		int v1_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[1];
		int v2_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[2];

		glm::vec3 A = sceneFaces[k].model->vertices[v0_k_index];
		glm::vec3 B = sceneFaces[k].model->vertices[v1_k_index];
		glm::vec3 C = sceneFaces[k].model->vertices[v2_k_index];

		glm::vec3 hitPoint;
		RayHit currentHit;
		if (sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes.size() > 3) {
			int v3_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[3];
			glm::vec3 D = sceneFaces[k].model->vertices[v3_k_index];

			if (glm::intersectRayTriangle(input.getStart(), input.getDirection(), A, B, D, hitPoint)) {
				currentHit.distance = glm::distance(ray.getStart(), hitPoint);
				currentHit.hitpoint = hitPoint;
				currentHit.hitSceneFaceIndex = k;
				hitPoint = glm::vec3(0.0f);
			}
			else if (glm::intersectRayTriangle(input.getStart(), input.getDirection(), C, B, D, hitPoint)) {
				currentHit.distance = glm::distance(ray.getStart(), hitPoint);
				currentHit.hitpoint = hitPoint;
				currentHit.hitSceneFaceIndex = k;
				rayHits.push_back(currentHit);
				hitPoint = glm::vec3(0.0f);
			}
			else if (glm::intersectRayTriangle(input.getStart(), input.getDirection(), A, C, D, hitPoint)) {
				currentHit.distance = glm::distance(ray.getStart(), hitPoint);
				currentHit.hitpoint = hitPoint;
				currentHit.hitSceneFaceIndex = k;
				rayHits.push_back(currentHit);
				hitPoint = glm::vec3(0.0f);
			}
			else if (glm::intersectRayTriangle(input.getStart(), input.getDirection(), A, B, C, hitPoint)) {
				currentHit.distance = glm::distance(ray.getStart(), hitPoint);
				currentHit.hitpoint = hitPoint;
				currentHit.hitSceneFaceIndex = k;
				rayHits.push_back(currentHit);
				hitPoint = glm::vec3(0.0f);
			}

			hitPoint = glm::vec3(0.0f);
		}
		else if (glm::intersectRayTriangle(input.getStart(), input.getDirection(), A, B, C, hitPoint)) {
			currentHit.distance = glm::distance(ray.getStart(), hitPoint);
			currentHit.hitpoint = hitPoint;
			currentHit.hitSceneFaceIndex = k;
			rayHits.push_back(currentHit);
		}


	}

	if (rayHits.size() >= 1) {
		global_k = rayHits[0].hitSceneFaceIndex;
		float distance = rayHits[0].distance;
		global_distance = distance;
		r_ij = rayHits[0].hitpoint;

		for (int j = 1; j < rayHits.size(); j++) {

			if (rayHits[j].distance < distance) {
				global_k = rayHits[j].hitSceneFaceIndex;
				distance = rayHits[j].distance;
				global_distance = distance;
				r_ij = rayHits[j].hitpoint;

			}
		}

	}

	// return's the values if it hits
	return (rayHits.size() == 1);

}



bool Radiosity::doesRayHit(Ray* ray, int k, glm::vec3& hitPoint)
{
	//check if ray is parallel to face
	glm::vec3 n_k = sceneFaces[k].model->getFaceNormal(sceneFaces[k].faceIndex);

	//if(isParallelToFace(ray, k))
	//	return false;

	//get all vertices of k
	int v0_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[0];
	int v1_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[1];
	int v2_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[2];

	glm::vec3 A = sceneFaces[k].model->vertices[v0_k_index];
	glm::vec3 B = sceneFaces[k].model->vertices[v1_k_index];
	glm::vec3 C = sceneFaces[k].model->vertices[v2_k_index];

	//first we handle the case where patch k has only 3 vertices

	//plane equasion
	//ax + by + cz = d
	//n.x=d
	float a = n_k.x;
	float b = n_k.y;
	float c = n_k.z;
	float d = glm::dot(n_k, A);

	//ray equasion
	//R(t) = ray.start + t*ray.direction
	//plug into plane equasion, solve for t

	float t = (d - glm::dot(n_k, ray->getStart())) / glm::dot(n_k, ray->getDirection());

	//triangle behind ray
	if (t<0.0f)
		return false;

	//calculate the intersection point between the ray and the plane where k lies
	glm::vec3 Q = ray->getStart() + (t * ray->getDirection());

	//now we determine if Q is inside or outside of k
	if (sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes.size() == 3)
	{
		float AB_EDGE = glm::dot(glm::cross((B - A), (Q - A)), n_k);
		float BC_EDGE = glm::dot(glm::cross((C - B), (Q - B)), n_k);
		float CA_EDGE = glm::dot(glm::cross((A - C), (Q - C)), n_k);

		if (AB_EDGE >= 0.0f && BC_EDGE >= 0.0f && CA_EDGE >= 0.0f)
		{
			float ABC_AREA_DOUBLE = glm::dot(glm::cross((B - A), (C - A)), n_k);

			float alpha = BC_EDGE / ABC_AREA_DOUBLE;
			float beta = CA_EDGE / ABC_AREA_DOUBLE;
			float gamma = AB_EDGE / ABC_AREA_DOUBLE;

			hitPoint = glm::vec3(alpha * A + beta * B + gamma * C);
			return true;
		}
		else
			return false;
	}
	else
	{
		int v3_k_index = sceneFaces[k].model->faces[sceneFaces[k].faceIndex].vertexIndexes[3];
		glm::vec3 D = sceneFaces[k].model->vertices[v3_k_index];

		float AB_EDGE = glm::dot(glm::cross((B - A), (Q - A)), n_k);
		float BD_EDGE = glm::dot(glm::cross((D - B), (Q - B)), n_k);
		float DA_EDGE = glm::dot(glm::cross((A - D), (Q - D)), n_k);

		float BC_EDGE = glm::dot(glm::cross((C - B), (Q - B)), n_k);
		float CD_EDGE = glm::dot(glm::cross((D - C), (Q - C)), n_k);
		float DB_EDGE = glm::dot(glm::cross((B - D), (Q - D)), n_k);

		if (AB_EDGE >= 0.0f && BD_EDGE >= 0.0f && DA_EDGE >= 0.0f)
		{
			float ABD_AREA_DOUBLE = glm::dot(glm::cross((B - A), (D - A)), n_k);

			float alpha = BD_EDGE / ABD_AREA_DOUBLE;
			float beta = DA_EDGE / ABD_AREA_DOUBLE;
			float gamma = AB_EDGE / ABD_AREA_DOUBLE;

			hitPoint = glm::vec3(alpha * A + beta * B + gamma * D);
			return true;
		}
		else if (BC_EDGE >= 0.0f && CD_EDGE >= 0.0f && DB_EDGE >= 0.0f)
		{
			float BCD_AREA_DOUBLE = glm::dot(glm::cross((B - C), (D - C)), n_k);

			float alpha = CD_EDGE / BCD_AREA_DOUBLE;
			float beta = DB_EDGE / BCD_AREA_DOUBLE;
			float gamma = BC_EDGE / BCD_AREA_DOUBLE;

			hitPoint = glm::vec3(alpha * B + beta * C + gamma * D);
			return true;
		}
		else
			return false;
	}
	return false;
}